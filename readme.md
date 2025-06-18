# ˫����ͷͬ��������RTSP��ý��ϵͳ

## ��Ŀ����
��ϵͳʵ����һ����Ч��˫USB����ͷͬ��������RTSP��ý�������������Ĺ��ܰ�����
- ˫����ͷӲ��ͬ����ʼ��
- ����ʱ�����֡��ͬ��
- ���ӳ�RTSP��ý�崫��
- �����ڴ��������
- �ͻ�������״̬����

## ����ջ���
- **GStreamer**������RTSP��ý�崫��
  - appsrc���Զ�������Դ
  - x264enc��H.264����
  - rtph264pay��RTP��װ
- **FFmpeg**��������Ƶ�����֡����
  - libavcodec����Ƶ����
  - libavformat���豸����
  - libswscale�����ظ�ʽת��
- **C++14����**��
  - ���߳�(std::thread)
  - ԭ�Ӳ���(std::atomic)
  - ����ָ��(std::shared_ptr)
- **ϵͳ���Ż�**��
  - �ڴ������������
  - �㿽��֡����
  - ���ӳ�����

## �ܹ������

### �������
1. **DualCameraCapture (sync_camera.cpp/h)**
   - ��������ͷ��ʼ����֡�����ͬ��
   - ʵ��YUV420P֡ת��
   - ����ʱ����Ķ�����ͷ֡ͬ��
   - �����ڴ��غͽ����������

2. **��Ƶ�������� (video_streamer.cpp/h)**
   - ʹ��GStreamerʵ��RTSP������
   - ��YUV420P֡����ΪH.264��
   - ʵ�ֿͻ�������״̬����
   - ֧��PTS(��ʾʱ���)��̬����

### �߳�ģ��
1. **���߳�**��ϵͳ��ʼ����״̬���
2. **����ͷ�����߳�**��ÿ������ͷ�����߳�(��2��)
3. **֡ͬ���߳�**��ƥ��˫����ͷʱ���
4. **RTSP�����߳�**��GStreamer��ѭ��
5. **֡�����߳�**����GStreamer�ܵ�ι����

### �ڴ����ϵͳ
```mermaid
graph LR
A[֡����] --> B[֡����]
B --> C{�ڴ���}
C -->|������ֵ| D[��������]
C -->|������Χ| E[ͬ������]
E --> F[RTSP����]
```

## ��������

### ��ʼ������
1. ����`DualCameraCapture`ʵ������ʼ��˫����ͷ
2. ��������`VideoStreamer`ʵ��(RTSP������)
3. �������й����߳�

### ������
1. **����׶�**��
   - ����ͷ�߳�ͨ��FFmpeg����ԭʼ֡
   - ת��ΪYUV420P��ʽ�����ʱ���
   - ֡�����ʱ����Ķ���

2. **ͬ���׶�**��
   - ͬ���̱߳Ƚ�˫����ͷ֡ʱ���
   - ʱ�������<100��s��֡���Ϊͬ��֡
   - ͬ��֡���봫�����

3. **������׶�**��
   - ���ͻ�������״̬
   - ��̬�����¿ͻ��˵�PTS
   - ͨ��GStreamer�ܵ�����ΪH.264
   - ͨ��RTP/RTSP����

## ���ܿͻ��˹���
- **�����ڻ���**���׿ͻ������Ӻ�ȴ�10���õڶ��ͻ��˼���
- **PTS����**���¿ͻ�������ʱ����ʱ�������
- **����״̬�ص�**��ʵʱ֪ͨ���ӱ仯
```mermaid
sequenceDiagram
    participant C as Client
    participant S as Server
    C->>S: ����RTSP
    S->>S: ���������ڼ�ʱ��(10��)
    S->>S: ���ڶ��ͻ���
    alt 10�����еڶ��ͻ���
        S->>S: ����˫����ͷ
    else ��ʱ
        S->>S: ���ͻ���ģʽ����
    end
    S->>C: ������Ƶ��
```

## �ڴ����
- **ȫ�ָ���**��
  ```cpp
  std::atomic<size_t> g_total_frames_allocated{0};
  std::atomic<size_t> g_total_frames_freed{0};
  std::atomic<size_t> g_active_frame_count{0};
  ```
- **��������**��
  ```cpp
  size_t emergency_memory_cleanup() {
    // ��������2֡�������֡
  }
  ```
- **����ƽ��**��
  ```cpp
  size_t balance_frame_queues() {
    // ��ֹ��������֡������
  }
  ```

## ����������
### ������
- FFmpeg 4.4+
- GStreamer 1.18+
- C++14���ݱ�����

### ��������
```bash
g++ -std=c++14 main.cpp sync_camera.cpp video_streamer.cpp \
    -o dual_cam_streamer \
    $(pkg-config --cflags --libs libavcodec libavformat libswscale gstreamer-1.0)
```

### ����
```bash
./dual_cam_streamer
```

### ��������
```bash
# ������ͷ��
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay rtsp://192.168.16.247:5004/stream

# ������ͷ��
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay rtsp://192.168.16.247:5006/stream
```

## ϵͳ���
����ʱ����̨��ʾ�ؼ�ָ�꣺
```
[STATUS] Runtime: 5min, Memory: 45MB (peak: 89MB)
[CAMERA] Pairs sent: 8500 (success: 8490, fail: 10)
[QUEUE] Active frames: 12, Sync queue: 2
```

## ���ϴ���
1. **���ڴ�ʹ��**��
   - ϵͳ�Զ�������������
   - �ֶ�����SIGTERM�źŰ�ȫ����

2. **�ͻ��˶���**��
   - �Զ���ͣ����ͷ����
   - ���ֵ��ڴ�ռ�ô���״̬

3. **ͬ��ʧ��**��
   - �Զ������޷�ƥ���֡
   - ��̬����ͬ����ֵ
```
