#!/bin/bash

echo "========================================"
echo "SRT客户端连接测试 (Linux)"
echo "========================================"
echo ""

SERVER_IP="192.168.16.50"
PORT_FRONT="5010"
PORT_BACK="5011"

echo "[检查1] 网络连通性测试..."
ping -c 2 $SERVER_IP
if [ $? -ne 0 ]; then
    echo "[错误] 无法ping通服务器 $SERVER_IP"
    exit 1
fi
echo ""

echo "[检查2] 端口连通性测试..."
nc -zvu $SERVER_IP $PORT_FRONT 2>&1
nc -zvu $SERVER_IP $PORT_BACK 2>&1
echo ""

echo "[检查3] SRT连接测试（前置摄像头 - 端口 $PORT_FRONT）..."
echo "命令: ffplay -fflags nobuffer -flags low_delay -framedrop \"srt://$SERVER_IP:$PORT_FRONT?mode=caller&latency=200\""
echo ""
echo "按 Ctrl+C 停止播放"
echo ""

ffplay -fflags nobuffer -flags low_delay -framedrop "srt://$SERVER_IP:$PORT_FRONT?mode=caller&latency=200"

echo ""
echo "========================================"
echo "测试完成"
echo "========================================"
