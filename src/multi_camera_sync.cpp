#include "../includes/multi_camera_sync.h"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <chrono>

extern "C" {
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
}

//==============================================================================
// MultiCameraCapture 实现
//==============================================================================

MultiCameraCapture::MultiCameraCapture() {
    avdevice_register_all();
}

MultiCameraCapture::~MultiCameraCapture() {
    stop();
    for (size_t i = 0; i < cameras_.size(); ++i) {
        cleanup_camera(static_cast<int>(i));
    }
}

bool MultiCameraCapture::init(const std::vector<std::string>& device_paths, const SyncConfig& config) {
    if (device_paths.empty()) return false;

    camera_count_ = device_paths.size();
    config_ = config;

    cameras_.resize(camera_count_);
    frame_queues_.resize(camera_count_);

    for (size_t i = 0; i < camera_count_; ++i) {
        if (!init_camera(static_cast<int>(i), device_paths[i])) {
            std::cerr << "Failed to init camera " << i << std::endl;
            return false;
        }
    }

    initialized_ = true;
    return true;
}

bool MultiCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];
    cam.index = index;
    cam.device_path = device_path;

    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "rtbufsize", "100K", 0);  // 最小化缓冲，只保留几帧
    av_dict_set(&options, "pixel_format", "yuyv422", 0);

    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);

    if (ret != 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, AV_ERROR_MAX_STRING_SIZE);
        std::cerr << "Camera " << index << " init failed: " << error_buf << " (code: " << ret << ")" << std::endl;
        av_dict_free(&options);
        return false;
    }

    av_dict_free(&options);

    if (avformat_find_stream_info(cam.fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info for camera " << index << std::endl;
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    // 查找视频流
    cam.video_stream_idx = -1;
    for (unsigned int i = 0; i < cam.fmt_ctx->nb_streams; i++) {
        if (cam.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            cam.video_stream_idx = i;
            break;
        }
    }

    if (cam.video_stream_idx == -1) {
        std::cerr << "No video stream found for camera " << index << std::endl;
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    // 其余初始化代码
    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Could not find decoder for camera " << index << std::endl;
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) {
        std::cerr << "Could not allocate codec context for camera " << index << std::endl;
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) {
        std::cerr << "Could not copy codec params for camera " << index << std::endl;
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec for camera " << index << std::endl;
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    // 分配帧和转换上下文
    cam.frame = av_frame_alloc();
    cam.yuv_frame = av_frame_alloc();
    if (!cam.frame || !cam.yuv_frame) {
        std::cerr << "Could not allocate frames for camera " << index << std::endl;
        return false;
    }

    // USB摄像头固定使用640x480分辨率
    int target_width = 640;
    int target_height = 480;

    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, target_width, target_height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) {
        std::cerr << "Could not allocate YUV buffer for camera " << index << std::endl;
        return false;
    }

    av_image_fill_arrays(cam.yuv_frame->data, cam.yuv_frame->linesize,
        cam.yuv_buffer, AV_PIX_FMT_YUV420P, target_width, target_height, 32);

    cam.yuv_frame->width = target_width;
    cam.yuv_frame->height = target_height;
    cam.yuv_frame->format = AV_PIX_FMT_YUV420P;

    cam.sws_ctx_yuv = sws_getContext(
        cam.codec_ctx->width, cam.codec_ctx->height, cam.codec_ctx->pix_fmt,
        target_width, target_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!cam.sws_ctx_yuv) {
        std::cerr << "Could not create SWS context for camera " << index << std::endl;
        return false;
    }

    return true;
}

void MultiCameraCapture::start() {
    if (!initialized_ || running_.load()) return;

    running_ = true;
    capture_active_ = true;

    capture_threads_.reserve(camera_count_);
    for (size_t i = 0; i < camera_count_; ++i) {
        capture_threads_.emplace_back(&MultiCameraCapture::capture_thread, this, static_cast<int>(i));
    }

    sync_thread_ = std::thread(&MultiCameraCapture::sync_loop, this);
}

void MultiCameraCapture::stop() {
    if (!running_.load()) return;

    running_ = false;
    capture_active_ = false;

    for (auto& t : capture_threads_) {
        if (t.joinable()) t.join();
    }
    if (sync_thread_.joinable()) sync_thread_.join();

    capture_threads_.clear();

    // 清理队列
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (auto& queue : frame_queues_) {
        while (!queue.empty()) {
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
        }
    }
    while (!synced_frame_queue_.empty()) {
        auto& frames = synced_frame_queue_.front();
        for (auto* frame : frames) {
            free_cloned_frame(&frame);
        }
        synced_frame_queue_.pop_front();
    }
}

void MultiCameraCapture::capture_thread(int camera_index) {
    if (camera_index < 0 || static_cast<size_t>(camera_index) >= cameras_.size()) return;

    auto& cam = cameras_[static_cast<size_t>(camera_index)];
    AVPacket* packet = av_packet_alloc();
    if (!packet) return;

    uint64_t last_frame_checksum = 0;
    int identical_frame_count = 0;
    int frame_captured_count = 0;
    int read_failed_count = 0;
    
    auto last_capture_time = std::chrono::steady_clock::now();
    int64_t total_interval_us = 0;

    while (running_) {
        if (!capture_active_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        int ret = av_read_frame(cam.fmt_ctx, packet);
        if (ret < 0) {
            av_packet_unref(packet);
            read_failed_count++;
            if (read_failed_count % 1000 == 0) {
                char error_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, error_buf, AV_ERROR_MAX_STRING_SIZE);
                std::cerr << "Camera " << camera_index << " read failed " << read_failed_count << " times: " << error_buf << std::endl;
            }
            continue;
        }

        if (packet->stream_index != cam.video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(cam.codec_ctx, packet);
        av_packet_unref(packet);
        if (ret < 0) continue;

        while (true) {
            ret = avcodec_receive_frame(cam.codec_ctx, cam.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // 转换为YUV420P
            if (cam.sws_ctx_yuv) {
                int sws_ret = sws_scale(cam.sws_ctx_yuv,
                    (const uint8_t* const*)cam.frame->data, cam.frame->linesize,
                    0, cam.frame->height,
                    cam.yuv_frame->data, cam.yuv_frame->linesize);

                if (sws_ret > 0) {
                    cam.yuv_frame->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(camera_index + 1));

                    // 🔍 计算帧内容校验和
                    uint64_t current_checksum = 0;
                    for (int i = 0; i < cam.yuv_frame->height; i += 10) {  // 每10行采样一次
                        for (int j = 0; j < cam.yuv_frame->width; j += 10) { // 每10列采样一次
                            if (i * cam.yuv_frame->linesize[0] + j < cam.yuv_frame->linesize[0] * cam.yuv_frame->height) {
                                current_checksum += cam.yuv_frame->data[0][i * cam.yuv_frame->linesize[0] + j];
                            }
                        }
                    }

                    // 检查是否是重复帧
                    if (current_checksum == last_frame_checksum) {
                        identical_frame_count++;
                    }
                    else {
                       
                        identical_frame_count = 0;
                        last_frame_checksum = current_checksum;
                    }

                    // USB摄像头同步模式处理
                    int64_t timestamp_us = av_gettime();
                    
                    // 计算采集间隔
                    auto now = std::chrono::steady_clock::now();
                    auto interval_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_capture_time).count();
                    last_capture_time = now;
                    if (frame_captured_count > 0) {
                        total_interval_us += interval_us;
                    }

                    // 克隆帧用于同步队列
                    AVFrame* cloned_frame = clone_frame(cam.yuv_frame);
                    if (cloned_frame) {
                        TimestampedFrame timestamped_frame;
                        timestamped_frame.frame = cloned_frame;
                        timestamped_frame.timestamp_us = timestamp_us;

                        {
                            std::lock_guard<std::mutex> lock(queue_mutex_);

                            while (frame_queues_[camera_index].size() >= config_.max_queue_size) {
                                auto& old_timestamped_frame = frame_queues_[camera_index].front();
                                free_cloned_frame(&old_timestamped_frame.frame);
                                frame_queues_[camera_index].pop_front();
                            }

                            frame_queues_[camera_index].push_back(timestamped_frame);
                            frame_captured_count++;
                        }

                        // 🔍 每30帧输出采集队列大小
                        if (frame_captured_count % 30 == 0) {
                            std::lock_guard<std::mutex> lock(queue_mutex_);
                            std::cout << "[Camera " << camera_index << "] Captured=" << frame_captured_count 
                                      << " | Queue size=" << frame_queues_[camera_index].size() << std::endl;
                        }
                    } else {
                        if (frame_captured_count % 100 == 1) {
                            std::cerr << "Camera " << camera_index << " failed to clone frame!" << std::endl;
                        }
                    }
                }
            }
        }
    }

    std::cout << "[Camera " << camera_index << "] Total: " << frame_captured_count << " frames" << std::endl;

    av_packet_free(&packet);
}

void MultiCameraCapture::sync_loop() {
    const auto tick = std::chrono::milliseconds(1);
    const size_t N = frame_queues_.size();
    if (N == 0) return;

    const int64_t tolerance = config_.sync_threshold_us;
    int synced_count = 0;

    while (running_) {
        // 批量处理5帧,减少锁竞争但不过度缓冲
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            
            // 批量同步5帧,保持同步队列有适度缓冲
            int batch_count = 0;
            while (batch_count < 5 && synced_frame_queue_.size() < config_.max_sync_queue_size) {
                std::vector<AVFrame*> group(N, nullptr);
                bool all_have_frames = true;
                std::vector<int64_t> heads(N);

                for (size_t i = 0; i < N; ++i) {
                    if (frame_queues_[i].empty()) {
                        all_have_frames = false;
                        break;
                    }
                    heads[i] = frame_queues_[i].front().timestamp_us;
                }

                if (!all_have_frames) {
                    break;  // 等待下次循环
                }

                int64_t t_min = *std::min_element(heads.begin(), heads.end());
                int64_t t_max = *std::max_element(heads.begin(), heads.end());

                if (t_max - t_min <= tolerance) {
                    // 时间戳匹配,同步这组帧
                    for (size_t i = 0; i < N; ++i) {
                        auto& timestamped_frame = frame_queues_[i].front();
                        group[i] = timestamped_frame.frame;
                        frame_queues_[i].pop_front();
                    }

                    synced_frame_queue_.push_back(group);
                    synced_count++;
                    batch_count++;
                    
                    if (synced_count % 300 == 1) {
                        std::cout << "[Sync] " << synced_count << " groups synced" << std::endl;
                    }
                }
                else {
                    // 时间戳不匹配,丢弃过旧的帧
                    int64_t drop_threshold = t_max - tolerance;

                    for (size_t i = 0; i < N; ++i) {
                        while (!frame_queues_[i].empty() &&
                            frame_queues_[i].front().timestamp_us < drop_threshold) {

                            auto& old_timestamped_frame = frame_queues_[i].front();
                            AVFrame* old_frame = old_timestamped_frame.frame;
                            free_cloned_frame(&old_frame);
                            frame_queues_[i].pop_front();
                        }
                    }
                    break;  // 丢帧后重新检查
                }
            }
        } // lock自动释放
        
        // sleep 5ms,每秒最多同步200次,每次最多10帧 = 2000帧/秒 >> 60帧/秒
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::vector<AVFrame*> MultiCameraCapture::get_sync_yuv420p_frames() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (synced_frame_queue_.empty()) return {};

    auto frames = synced_frame_queue_.front();
    synced_frame_queue_.pop_front();
    return frames;
}

void MultiCameraCapture::free_cloned_frame(AVFrame** frame) {
    if (frame && *frame) {
        if ((*frame)->data[0]) {
            av_freep(&(*frame)->data[0]);
        }
        av_frame_free(frame);
        *frame = nullptr;
    }
}

AVFrame* MultiCameraCapture::clone_frame(const AVFrame* src) const {
    if (!src) return nullptr;

    AVFrame* dst = av_frame_alloc();
    if (!dst) return nullptr;

    dst->format = src->format;
    dst->width = src->width;
    dst->height = src->height;

    int ret = av_image_alloc(dst->data, dst->linesize,
        dst->width, dst->height, (AVPixelFormat)dst->format, 32);
    if (ret < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    av_image_copy(dst->data, dst->linesize,
        (const uint8_t* const*)src->data, src->linesize,
        (AVPixelFormat)dst->format, dst->width, dst->height);

    dst->pts = src->pts;
    dst->opaque = src->opaque;
    return dst;
}

void MultiCameraCapture::release_frame(AVFrame** frame) {
    free_cloned_frame(frame);
}

void MultiCameraCapture::cleanup_camera(int index) {
    if (index < 0 || static_cast<size_t>(index) >= cameras_.size()) return;

    auto& cam = cameras_[static_cast<size_t>(index)];

    if (cam.sws_ctx_yuv) {
        sws_freeContext(cam.sws_ctx_yuv);
        cam.sws_ctx_yuv = nullptr;
    }

    if (cam.yuv_frame) av_frame_free(&cam.yuv_frame);
    if (cam.frame) av_frame_free(&cam.frame);
    if (cam.yuv_buffer) av_freep(&cam.yuv_buffer);
    if (cam.codec_ctx) avcodec_free_context(&cam.codec_ctx);
    if (cam.fmt_ctx) avformat_close_input(&cam.fmt_ctx);

    cam.video_stream_idx = -1;
}

size_t MultiCameraCapture::get_sync_queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return synced_frame_queue_.size();
}

size_t MultiCameraCapture::get_camera_count() const {
    return camera_count_;
}
