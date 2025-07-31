// improved_camera_mjpeg_test.cpp - 正确的MJPEG初始化方案
#include <iostream>
#include <vector>
#include <string>
#include <map>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
void setup_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep(ms * 1000)
void setup_console() {}
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

struct CameraSpec {
    std::string name;
    std::vector<std::pair<int, int>> mjpeg_resolutions;  // width, height
    int max_fps;
};

class MJPEGCameraInitializer {
public:
    MJPEGCameraInitializer() {
        setup_console();
        avdevice_register_all();
        av_log_set_level(AV_LOG_INFO);
        
        // 初始化摄像头规格
        init_camera_specs();
    }

private:
    std::vector<CameraSpec> camera_specs;

    void init_camera_specs() {
        // 基于你的ffmpeg输出分析的MJPEG支持规格
        camera_specs = {
            {"USB Camera", {{2560, 720}, {1280, 720}, {1280, 712}, {1280, 480}, {640, 480}, {640, 472}}, 30},
            {"720P USB Camera", {{1280, 720}, {640, 480}, {640, 360}}, 30},
            {"1080P USB Camera", {{1920, 1080}, {1280, 960}, {1280, 720}, {640, 480}, {640, 360}}, 30}
        };
    }

    // 方案1：正确的参数组合 - 关键是参数顺序和组合
    bool try_correct_parameter_combination(const std::string& camera_name, const CameraSpec& spec) {
        std::cout << "方案1: 正确参数组合法" << std::endl;
        
        for (const auto& resolution : spec.mjpeg_resolutions) {
            std::cout << "  尝试 " << resolution.first << "x" << resolution.second << std::endl;
            
            const AVInputFormat* input_format = av_find_input_format("dshow");
            AVFormatContext* fmt_ctx = nullptr;
            AVDictionary* options = nullptr;
            
            // 关键：正确的参数组合和顺序
            std::string size_str = std::to_string(resolution.first) + "x" + std::to_string(resolution.second);
            std::string fps_str = std::to_string(spec.max_fps);
            
            // 这个组合很重要：先设置视频编解码器，再设置尺寸和帧率
            av_dict_set(&options, "vcodec", "mjpeg", 0);
            av_dict_set(&options, "video_size", size_str.c_str(), 0);
            av_dict_set(&options, "framerate", fps_str.c_str(), 0);
            av_dict_set(&options, "rtbufsize", "100000000", 0);  // 增大缓冲区
            
            std::string device_path = "video=" + camera_name;
            int ret = avformat_open_input(&fmt_ctx, device_path.c_str(), input_format, &options);
            
            if (ret == 0) {
                if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
                    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                            std::string codec_name = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
                            std::cout << "    结果: " << codec_name << " " 
                                      << fmt_ctx->streams[i]->codecpar->width << "x" 
                                      << fmt_ctx->streams[i]->codecpar->height << std::endl;
                            
                            if (codec_name == "mjpeg") {
                                av_dict_free(&options);
                                avformat_close_input(&fmt_ctx);
                                return true;
                            }
                        }
                    }
                }
                avformat_close_input(&fmt_ctx);
            }
            av_dict_free(&options);
        }
        return false;
    }

    // 方案2：像素格式强制指定法
    bool try_pixel_format_specification(const std::string& camera_name, const CameraSpec& spec) {
        std::cout << "方案2: 像素格式强制指定法" << std::endl;
        
        // 尝试强制指定MJPEG相关的像素格式
        std::vector<std::string> pixel_formats = {"mjpeg", "yuvj420p", "yuvj422p"};
        
        for (const auto& pix_fmt : pixel_formats) {
            std::cout << "  尝试像素格式: " << pix_fmt << std::endl;
            
            const AVInputFormat* input_format = av_find_input_format("dshow");
            AVFormatContext* fmt_ctx = nullptr;
            AVDictionary* options = nullptr;
            
            // 使用较小的分辨率更容易成功
            av_dict_set(&options, "pixel_format", pix_fmt.c_str(), 0);
            av_dict_set(&options, "video_size", "640x480", 0);
            av_dict_set(&options, "framerate", "30", 0);
            
            std::string device_path = "video=" + camera_name;
            int ret = avformat_open_input(&fmt_ctx, device_path.c_str(), input_format, &options);
            
            if (ret == 0) {
                if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
                    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                            std::string codec_name = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
                            std::cout << "    结果: " << codec_name << " " 
                                      << fmt_ctx->streams[i]->codecpar->width << "x" 
                                      << fmt_ctx->streams[i]->codecpar->height << std::endl;
                            
                            if (codec_name == "mjpeg") {
                                av_dict_free(&options);
                                avformat_close_input(&fmt_ctx);
                                return true;
                            }
                        }
                    }
                }
                avformat_close_input(&fmt_ctx);
            }
            av_dict_free(&options);
        }
        return false;
    }

    // 方案3：两步初始化法
    bool try_two_step_initialization(const std::string& camera_name, const CameraSpec& spec) {
        std::cout << "方案3: 两步初始化法" << std::endl;
        
        // 第一步：用MJPEG参数打开，但允许失败
        const AVInputFormat* input_format = av_find_input_format("dshow");
        AVFormatContext* fmt_ctx = nullptr;
        AVDictionary* options = nullptr;
        
        std::cout << "  第一步：MJPEG参数尝试" << std::endl;
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "25", 0);  // 使用较低帧率
        
        std::string device_path = "video=" + camera_name;
        int ret = avformat_open_input(&fmt_ctx, device_path.c_str(), input_format, &options);
        av_dict_free(&options);
        
        if (ret == 0) {
            std::cout << "  第一步成功，检查流信息" << std::endl;
            if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
                for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        std::string codec_name = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
                        std::cout << "    结果: " << codec_name << " " 
                                  << fmt_ctx->streams[i]->codecpar->width << "x" 
                                  << fmt_ctx->streams[i]->codecpar->height << std::endl;
                        
                        if (codec_name == "mjpeg") {
                            avformat_close_input(&fmt_ctx);
                            return true;
                        }
                    }
                }
            }
            avformat_close_input(&fmt_ctx);
        } else {
            std::cout << "  第一步失败，尝试第二步" << std::endl;
        }
        
        // 第二步：如果第一步失败，使用备用参数
        fmt_ctx = nullptr;
        options = nullptr;
        
        av_dict_set(&options, "list_options", "false", 0);
        av_dict_set(&options, "video_size", "320x240", 0);  // 更小的尺寸
        av_dict_set(&options, "framerate", "15", 0);
        av_dict_set(&options, "rtbufsize", "50000000", 0);
        
        ret = avformat_open_input(&fmt_ctx, device_path.c_str(), input_format, &options);
        av_dict_free(&options);
        
        if (ret == 0) {
            if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
                for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        std::string codec_name = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
                        std::cout << "  第二步结果: " << codec_name << " " 
                                  << fmt_ctx->streams[i]->codecpar->width << "x" 
                                  << fmt_ctx->streams[i]->codecpar->height << std::endl;
                        
                        // 即使不是MJPEG，也记录结果
                        avformat_close_input(&fmt_ctx);
                        return (codec_name == "mjpeg");
                    }
                }
            }
            avformat_close_input(&fmt_ctx);
        }
        
        return false;
    }

    // 方案4：DirectShow选项深度控制法
    bool try_directshow_deep_control(const std::string& camera_name, const CameraSpec& spec) {
        std::cout << "方案4: DirectShow深度控制法" << std::endl;
        
        const AVInputFormat* input_format = av_find_input_format("dshow");
        AVFormatContext* fmt_ctx = nullptr;
        AVDictionary* options = nullptr;
        
        // DirectShow特定的高级选项
        av_dict_set(&options, "video_device_number", "0", 0);
        av_dict_set(&options, "audio_device_number", "-1", 0);  // 禁用音频
        av_dict_set(&options, "crossbar_video_input_pin_number", "0", 0);
        av_dict_set(&options, "crossbar_audio_input_pin_number", "-1", 0);
        av_dict_set(&options, "show_video_device_dialog", "false", 0);
        av_dict_set(&options, "use_video_device_timestamps", "true", 0);
        
        // MJPEG特定参数
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);
        
        // 缓冲区设置
        av_dict_set(&options, "rtbufsize", "200000000", 0);
        av_dict_set(&options, "buffer_size", "20000000", 0);
        
        std::string device_path = "video=" + camera_name;
        int ret = avformat_open_input(&fmt_ctx, device_path.c_str(), input_format, &options);
        
        // 检查哪些选项没有被使用
        if (options) {
            std::cout << "  未使用的选项: ";
            AVDictionaryEntry* entry = nullptr;
            while ((entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX))) {
                std::cout << entry->key << "=" << entry->value << " ";
            }
            std::cout << std::endl;
        }
        av_dict_free(&options);
        
        if (ret == 0) {
            if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
                for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        std::string codec_name = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
                        std::cout << "    结果: " << codec_name << " " 
                                  << fmt_ctx->streams[i]->codecpar->width << "x" 
                                  << fmt_ctx->streams[i]->codecpar->height << std::endl;
                        
                        if (codec_name == "mjpeg") {
                            avformat_close_input(&fmt_ctx);
                            return true;
                        }
                    }
                }
            }
            avformat_close_input(&fmt_ctx);
        }
        
        return false;
    }

    // 方案5：命令行参数完全复制法
    bool try_exact_command_replication(const std::string& camera_name) {
        std::cout << "方案5: 命令行参数完全复制法" << std::endl;
        
        // 完全按照成功的命令行顺序设置参数
        const AVInputFormat* input_format = av_find_input_format("dshow");
        AVFormatContext* fmt_ctx = nullptr;
        
        // 不使用AVDictionary，直接设置AVFormatContext
        fmt_ctx = avformat_alloc_context();
        if (!fmt_ctx) return false;
        
        // 直接设置输入格式
        fmt_ctx->iformat = input_format;
        
        // 设置URL
        std::string device_path = "video=" + camera_name;
        
        // 使用avformat_open_input的最简形式
        AVDictionary* options = nullptr;
        
        // 按命令行顺序精确设置
        av_dict_set(&options, "f", "dshow", 0);
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "video_size", "640x480", 0);
        
        int ret = avformat_open_input(&fmt_ctx, device_path.c_str(), input_format, &options);
        
        std::cout << "  avformat_open_input返回: " << ret << std::endl;
        
        if (options) {
            std::cout << "  剩余选项: ";
            AVDictionaryEntry* entry = nullptr;
            while ((entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX))) {
                std::cout << entry->key << "=" << entry->value << " ";
            }
            std::cout << std::endl;
        }
        av_dict_free(&options);
        
        if (ret == 0) {
            ret = avformat_find_stream_info(fmt_ctx, nullptr);
            std::cout << "  avformat_find_stream_info返回: " << ret << std::endl;
            
            if (ret >= 0) {
                for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        std::string codec_name = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
                        std::cout << "    最终结果: " << codec_name << " " 
                                  << fmt_ctx->streams[i]->codecpar->width << "x" 
                                  << fmt_ctx->streams[i]->codecpar->height << std::endl;
                        
                        if (codec_name == "mjpeg") {
                            avformat_close_input(&fmt_ctx);
                            return true;
                        }
                    }
                }
            }
            avformat_close_input(&fmt_ctx);
        } else {
            avformat_free_context(fmt_ctx);
        }
        
        return false;
    }

public:
    bool test_camera_mjpeg(const std::string& camera_name) {
        std::cout << std::endl;
        std::cout << "=== 测试摄像头: " << camera_name << " ===" << std::endl;
        
        // 找到对应的摄像头规格
        const CameraSpec* spec = nullptr;
        for (const auto& cs : camera_specs) {
            if (cs.name == camera_name) {
                spec = &cs;
                break;
            }
        }
        
        if (!spec) {
            std::cout << "未知摄像头: " << camera_name << std::endl;
            return false;
        }
        
        // 按优先级尝试各种方案
        if (try_correct_parameter_combination(camera_name, *spec)) {
            std::cout << "✓ 成功！使用方案1: 正确参数组合法" << std::endl;
            return true;
        }
        
        if (try_pixel_format_specification(camera_name, *spec)) {
            std::cout << "✓ 成功！使用方案2: 像素格式强制指定法" << std::endl;
            return true;
        }
        
        if (try_two_step_initialization(camera_name, *spec)) {
            std::cout << "✓ 成功！使用方案3: 两步初始化法" << std::endl;
            return true;
        }
        
        if (try_directshow_deep_control(camera_name, *spec)) {
            std::cout << "✓ 成功！使用方案4: DirectShow深度控制法" << std::endl;
            return true;
        }
        
        if (try_exact_command_replication(camera_name)) {
            std::cout << "✓ 成功！使用方案5: 命令行参数完全复制法" << std::endl;
            return true;
        }
        
        std::cout << "✗ 所有方案都失败了" << std::endl;
        return false;
    }

    void run_all_tests() {
        std::cout << "改进的DirectShow MJPEG初始化测试" << std::endl;
        std::cout << "=====================================" << std::endl;
        
        std::vector<std::string> cameras = {
            "USB Camera",
            "720P USB Camera", 
            "1080P USB Camera"
        };
        
        int success_count = 0;
        for (const auto& camera : cameras) {
            if (test_camera_mjpeg(camera)) {
                success_count++;
            }
            SLEEP_MS(1000);
        }
        
        std::cout << std::endl;
        std::cout << "=== 测试结果总结 ===" << std::endl;
        std::cout << "成功: " << success_count << "/" << cameras.size() << " 摄像头" << std::endl;
        
        if (success_count == 0) {
            std::cout << std::endl;
            std::cout << "=== 如果所有测试都失败 ===" << std::endl;
            std::cout << "1. 尝试更新FFmpeg版本（7.1+）" << std::endl;
            std::cout << "2. 检查DirectShow驱动状态" << std::endl;
            std::cout << "3. 考虑使用Media Foundation" << std::endl;
            std::cout << "4. 尝试第三方库如OpenCV" << std::endl;
        }
    }
};

int main() {
    try {
        MJPEGCameraInitializer tester;
        tester.run_all_tests();
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
    }
    
    std::cout << std::endl << "按Enter键退出..." << std::endl;
    std::cin.get();
    
    return 0;
}