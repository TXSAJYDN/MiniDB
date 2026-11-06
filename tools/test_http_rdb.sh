#!/bin/bash
# MiniDB HTTP RDB 功能端到端测试

set -e

PORT=8888
RDB_FILE="/tmp/minidb_e2e_test.rdb"
SERVER_PID=""

# 清理函数
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        echo "Stopping server (PID: $SERVER_PID)..."
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
    fi
    rm -f "$RDB_FILE"
    echo "Cleanup complete"
}

trap cleanup EXIT

echo "=== MiniDB HTTP RDB End-to-End Test ==="
echo ""

# 清理旧的 RDB 文件
rm -f "$RDB_FILE"

# 启动服务器
echo "[1/6] Starting MiniDB server on port $PORT..."
./minidb_http_server -p $PORT -r "$RDB_FILE" > /tmp/server.log 2>&1 &
SERVER_PID=$!
sleep 2

# 检查服务器是否启动
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: Server failed to start"
    cat /tmp/server.log
    exit 1
fi
echo "Server started (PID: $SERVER_PID)"
echo ""

# 测试健康检查
echo "[2/6] Testing health endpoint..."
response=$(curl -s http://localhost:$PORT/health)
if [ "$response" = "OK" ]; then
    echo "✓ Health check passed"
else
    echo "✗ Health check failed"
    exit 1
fi
echo ""

# 写入数据
echo "[3/6] Writing test data..."
curl -s -X POST -d "value1" http://localhost:$PORT/kv/key1 > /dev/null
curl -s -X POST -d "value2" http://localhost:$PORT/kv/key2 > /dev/null
curl -s -X POST -d "value3" http://localhost:$PORT/kv/key3 > /dev/null
echo "✓ Written 3 keys"
echo ""

# 读取数据验证
echo "[4/6] Verifying data..."
value1=$(curl -s http://localhost:$PORT/kv/key1)
if [ "$value1" = "value1" ]; then
    echo "✓ key1 = value1"
else
    echo "✗ key1 verification failed (got: $value1)"
    exit 1
fi
echo ""

# 触发 SAVE
echo "[5/6] Triggering RDB save..."
save_response=$(curl -s -X POST http://localhost:$PORT/save)
echo "Save response: $save_response"
if echo "$save_response" | grep -q '"status":"ok"'; then
    echo "✓ RDB save successful"
else
    echo "✗ RDB save failed"
    exit 1
fi

# 检查 RDB 文件是否存在
if [ -f "$RDB_FILE" ]; then
    size=$(stat -f%z "$RDB_FILE" 2>/dev/null || stat -c%s "$RDB_FILE" 2>/dev/null)
    echo "✓ RDB file created ($size bytes)"
else
    echo "✗ RDB file not found"
    exit 1
fi
echo ""

# 停止服务器
echo "[6/6] Restarting server to test recovery..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true
SERVER_PID=""
sleep 1

# 重新启动服务器（应该自动加载 RDB）
./minidb_http_server -p $PORT -r "$RDB_FILE" > /tmp/server2.log 2>&1 &
SERVER_PID=$!
sleep 2

# 验证数据已恢复
value1_restored=$(curl -s http://localhost:$PORT/kv/key1)
if [ "$value1_restored" = "value1" ]; then
    echo "✓ Data restored from RDB: key1 = value1"
else
    echo "✗ Data restoration failed (got: $value1_restored)"
    exit 1
fi

value2_restored=$(curl -s http://localhost:$PORT/kv/key2)
if [ "$value2_restored" = "value2" ]; then
    echo "✓ Data restored from RDB: key2 = value2"
else
    echo "✗ Data restoration failed"
    exit 1
fi
echo ""

echo "=== All tests passed! ==="
