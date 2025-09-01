#!/usr/bin/env python3
"""
真正的视频同步测试 - 检测内容级同步而非网络同步
"""

import cv2
import numpy as np
import socket
import time
from collections import deque
import hashlib

class TrueSyncTester:
    """真正的同步测试器 - 检测视频内容同步"""
    
    def __init__(self, ports: list, test_duration: int = 60):
        self.ports = ports
        self.test_duration = test_duration
        self.frame_data = {port: deque(maxlen=100) for port in ports}
        self.sync_results = []
        
    def i420_to_bgr(self, i420_data: bytes, width: int = 640, height: int = 480):
        """I420转BGR"""
        try:
            y_size = width * height
            uv_size = y_size // 4
            
            y = np.frombuffer(i420_data[:y_size], dtype=np.uint8).reshape((height, width))
            u = np.frombuffer(i420_data[y_size:y_size + uv_size], dtype=np.uint8).reshape((height // 2, width // 2))
            v = np.frombuffer(i420_data[y_size + uv_size:], dtype=np.uint8).reshape((height // 2, width // 2))
            
            u_full = cv2.resize(u, (width, height))
            v_full = cv2.resize(v, (width, height))
            
            yuv = np.stack([y, u_full, v_full], axis=2)
            return cv2.cvtColor(yuv.astype(np.uint8), cv2.COLOR_YUV2BGR)
        except Exception as e:
            print(f"I420 conversion error: {e}")
            return None
    
    def compute_frame_hash(self, frame_data: bytes) -> str:
        """计算帧的哈希值"""
        return hashlib.md5(frame_data).hexdigest()[:16]
    
    def detect_duplicate_frames(self, frames_group: list) -> dict:
        """检测重复帧"""
        hashes = [self.compute_frame_hash(frame) for frame in frames_group]
        unique_hashes = set(hashes)
        
        return {
            'total_frames': len(frames_group),
            'unique_frames': len(unique_hashes),
            'duplicate_rate': 1 - (len(unique_hashes) / len(frames_group)) if frames_group else 0,
            'hashes': hashes
        }
    
    def detect_black_frames(self, frames_group: list) -> dict:
        """检测黑帧"""
        black_count = 0
        brightness_values = []
        
        for frame_data in frames_group:
            bgr = self.i420_to_bgr(frame_data)
            if bgr is not None:
                brightness = np.mean(cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY))
                brightness_values.append(brightness)
                if brightness < 30:  # 很暗认为是黑帧
                    black_count += 1
        
        return {
            'black_frame_count': black_count,
            'black_frame_rate': black_count / len(frames_group) if frames_group else 0,
            'avg_brightness': np.mean(brightness_values) if brightness_values else 0,
            'brightness_std': np.std(brightness_values) if len(brightness_values) > 1 else 0
        }
    
    def analyze_visual_sync(self, frames_group: list) -> dict:
        """分析视觉同步性"""
        if len(frames_group) < 2:
            return {'error': 'Need at least 2 frames'}
        
        features_list = []
        
        for frame_data in frames_group:
            bgr = self.i420_to_bgr(frame_data)
            if bgr is None:
                continue
                
            gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
            
            # 修复角点检测
            try:
                corners = cv2.goodFeaturesToTrack(gray, maxCorners=100, qualityLevel=0.01, minDistance=10)
                corner_count = len(corners) if corners is not None else 0
            except Exception as e:
                print(f"Corner detection error: {e}")
                corner_count = 0
            
            # 提取多种特征
            try:
                edges = cv2.Canny(gray, 50, 150)
                edge_count = np.sum(edges == 255)
            except Exception as e:
                print(f"Edge detection error: {e}")
                edge_count = 0
            
            try:
                hist = cv2.calcHist([gray], [0], None, [64], [0, 256]).flatten()
            except Exception as e:
                print(f"Histogram calculation error: {e}")
                hist = np.zeros(64)
            
            features = {
                'brightness': np.mean(gray),
                'contrast': np.std(gray),
                'edges': edge_count,
                'corners': corner_count,
                'histogram': hist
            }
            features_list.append(features)
        
        if len(features_list) < 2:
            return {'error': 'Failed to extract features'}
        
        # 计算各特征的一致性
        correlations = {}
        
        # 简单特征相关性
        for feature in ['brightness', 'contrast', 'edges', 'corners']:
            try:
                values = [f[feature] for f in features_list]
                if len(set(values)) > 1:  # 有变化才计算相关性
                    # 计算变异系数（标准差/均值）
                    cv_value = np.std(values) / np.mean(values) if np.mean(values) > 0 else float('inf')
                    correlations[feature] = 1 / (1 + cv_value)  # 转换为相似性分数
                else:
                    correlations[feature] = 1.0  # 完全相同
            except Exception as e:
                print(f"Feature correlation error for {feature}: {e}")
                correlations[feature] = 0.0
        
        # 直方图相关性
        try:
            hist_correlations = []
            for i in range(len(features_list)):
                for j in range(i + 1, len(features_list)):
                    corr = cv2.compareHist(features_list[i]['histogram'], 
                                         features_list[j]['histogram'], 
                                         cv2.HISTCMP_CORREL)
                    hist_correlations.append(corr)
            
            correlations['histogram'] = np.mean(hist_correlations) if hist_correlations else 0
        except Exception as e:
            print(f"Histogram correlation error: {e}")
            correlations['histogram'] = 0.0
        
        return {
            'feature_correlations': correlations,
            'overall_similarity': np.mean(list(correlations.values())) if correlations else 0,
            'frame_count': len(features_list)
        }
    
    def detect_temporal_inconsistencies(self, camera_frames: dict) -> dict:
        """检测时间不一致性"""
        results = {}
        
        for camera_id, frames in camera_frames.items():
            if len(frames) < 3:
                continue
                
            # 连续帧差异分析
            frame_diffs = []
            for i in range(1, len(frames)):
                try:
                    curr_bgr = self.i420_to_bgr(frames[i])
                    prev_bgr = self.i420_to_bgr(frames[i-1])
                    
                    if curr_bgr is not None and prev_bgr is not None:
                        diff = cv2.absdiff(curr_bgr, prev_bgr)
                        diff_score = np.mean(diff)
                        frame_diffs.append(diff_score)
                except Exception as e:
                    print(f"Frame diff error: {e}")
                    continue
            
            if frame_diffs:
                # 检测异常静止（重复帧的迹象）
                very_low_diff = [d for d in frame_diffs if d < 1.0]  # 几乎无差异
                
                results[f'camera_{camera_id}'] = {
                    'avg_frame_diff': np.mean(frame_diffs),
                    'still_frame_rate': len(very_low_diff) / len(frame_diffs),
                    'motion_consistency': np.std(frame_diffs),
                    'suspicious_stillness': len(very_low_diff) / len(frame_diffs) > 0.5
                }
        
        return results
    
    def run_test(self) -> dict:
        """综合测试"""
        print(f"Starting comprehensive sync test for {len(self.ports)} cameras...")
        
        # 连接所有摄像头
        receivers = {}
        for port in self.ports:
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(5.0)
                sock.connect(('127.0.0.1', port))
                receivers[port] = {
                    'socket': sock,
                    'buffer': bytearray(),
                    'frames': []
                }
                print(f"✓ Connected to port {port}")
            except Exception as e:
                print(f"✗ Failed to connect to port {port}: {e}")
                return {'error': f'Connection failed: {port}'}
        
        # 收集数据
        start_time = time.time()
        frame_size = 640 * 480 * 3 // 2
        
        try:
            while time.time() - start_time < self.test_duration:
                sync_group_frames = {}
                sync_group_time = time.time()
                
                # 同时从所有端口读取一帧
                for port, receiver in receivers.items():
                    sock = receiver['socket']
                    buffer = receiver['buffer']
                    
                    # 尝试读取一帧
                    read_start = time.time()
                    while len(buffer) < frame_size and time.time() - read_start < 1.0:
                        try:
                            chunk = sock.recv(min(8192, frame_size - len(buffer)))
                            if not chunk:
                                break
                            buffer.extend(chunk)
                        except socket.timeout:
                            break
                        except Exception as e:
                            print(f"Socket error on port {port}: {e}")
                            break
                    
                    if len(buffer) >= frame_size:
                        frame_data = bytes(buffer[:frame_size])
                        buffer[:] = buffer[frame_size:]
                        sync_group_frames[port] = frame_data
                        receiver['frames'].append(frame_data)
                
                # 如果所有摄像头都有帧，进行同步分析
                if len(sync_group_frames) == len(self.ports):
                    frames_list = list(sync_group_frames.values())
                    
                    analysis = {
                        'timestamp': sync_group_time,
                        'duplicate_analysis': self.detect_duplicate_frames(frames_list),
                        'black_frame_analysis': self.detect_black_frames(frames_list),
                        'visual_sync_analysis': self.analyze_visual_sync(frames_list),
                        'frame_hashes': [self.compute_frame_hash(f) for f in frames_list]
                    }
                    
                    self.sync_results.append(analysis)
                    
                    # 每10个样本输出一次进度
                    if len(self.sync_results) % 10 == 0:
                        print(f"Collected {len(self.sync_results)} samples...")
                
                time.sleep(0.05)  # 20fps采样
                
        except KeyboardInterrupt:
            print("Test interrupted by user")
        except Exception as e:
            print(f"Test error: {e}")
        finally:
            for receiver in receivers.values():
                try:
                    receiver['socket'].close()
                except:
                    pass
        
        # 生成最终报告
        return self.generate_simple_report(receivers)
    
    def generate_simple_report(self, receivers: dict) -> dict:
        """生成简化报告"""
        if not self.sync_results:
            return {'error': 'No sync data collected'}
        
        try:
            # 重复帧统计
            duplicate_rates = [r['duplicate_analysis']['duplicate_rate'] for r in self.sync_results]
            
            # 黑帧统计  
            black_rates = [r['black_frame_analysis']['black_frame_rate'] for r in self.sync_results]
            
            # 视觉同步统计
            visual_similarities = [r['visual_sync_analysis'].get('overall_similarity', 0) 
                                 for r in self.sync_results 
                                 if 'overall_similarity' in r['visual_sync_analysis']]
            
            # 检测"作弊"指标
            avg_dup_rate = np.mean(duplicate_rates) if duplicate_rates else 0
            avg_black_rate = np.mean(black_rates) if black_rates else 0  
            avg_similarity = np.mean(visual_similarities) if visual_similarities else 0
            
            cheating_indicators = {
                'high_duplicate_rate': avg_dup_rate > 0.1,
                'excessive_black_frames': avg_black_rate > 0.05,
                'perfect_sync_suspicious': avg_similarity > 0.98,
            }
            
            return {
                'samples': len(self.sync_results),
                'duplicate_rate': avg_dup_rate,
                'black_rate': avg_black_rate,
                'similarity': avg_similarity,
                'cheating': cheating_indicators,
                'authenticity': self.calculate_authenticity_score(cheating_indicators)
            }
        except Exception as e:
            return {'error': f'Report generation failed: {e}'}
    
    def calculate_authenticity_score(self, cheating_indicators: dict) -> float:
        """计算真实性分数 (0-1, 1为完全真实)"""
        penalty_weights = {
            'high_duplicate_rate': 0.4,
            'excessive_black_frames': 0.3,
            'perfect_sync_suspicious': 0.3,
        }
        
        total_penalty = sum(
            weight for indicator, weight in penalty_weights.items()
            if cheating_indicators.get(indicator, False)
        )
        
        return max(0.0, 1.0 - total_penalty)

def main():
    """简单测试入口"""
    ports = [5010, 5011, 5012]  # 默认3摄像头
    duration = 30  # 默认30秒测试
    
    print(f"Testing ports: {ports}, Duration: {duration}s")
    
    tester = TrueSyncTester(ports, duration)
    report = tester.run_test()
    
    # 显示结果
    if 'error' not in report:
        print("\n" + "="*40)
        print("SYNC AUTHENTICITY TEST")
        print("="*40)
        
        auth_score = report['authenticity']
        print(f"🎯 Authenticity Score: {auth_score:.2f}/1.00")
        
        if auth_score > 0.8:
            print("✅ GENUINE - Real sync detected")
        elif auth_score > 0.6:
            print("⚠️  SUSPICIOUS - Possible fake sync")
        else:
            print("❌ FAKE SYNC - Strong evidence of cheating")
        
        print(f"\n📊 Detection Results:")
        print(f"   Samples analyzed: {report['samples']}")
        print(f"   Duplicate frame rate: {report['duplicate_rate']:.3f}")
        print(f"   Black frame rate: {report['black_rate']:.3f}")
        print(f"   Visual similarity: {report['similarity']:.3f}")
        
        cheating = report['cheating']
        print(f"\n🚨 Fraud Indicators:")
        print(f"   High duplicates: {'YES' if cheating['high_duplicate_rate'] else 'NO'}")
        print(f"   Excessive black frames: {'YES' if cheating['excessive_black_frames'] else 'NO'}")
        print(f"   Too perfect sync: {'YES' if cheating['perfect_sync_suspicious'] else 'NO'}")
    else:
        print(f"❌ Test failed: {report['error']}")

if __name__ == "__main__":
    main()