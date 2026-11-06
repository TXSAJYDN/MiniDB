#!/usr/bin/env python3
import socket
import struct
import sys

# 命令类型常量
CMD_PING = 1
CMD_SET = 2
CMD_GET = 3
CMD_DEL = 4
CMD_EXPIRE = 5
CMD_TTL = 6
CMD_ERROR = 100
CMD_OK = 101
CMD_VALUE = 102
CMD_NOTFOUND = 103
CMD_INTEGER = 104

def encode_command(cmd_type, *args):
    # 计算总长度：1字节命令类型 + 1字节参数数量 + 每个参数(4字节长度 + 数据)
    total_len = 1 + 1
    arg_data = []
    for arg in args:
        arg_bytes = arg.encode('utf-8') if isinstance(arg, str) else arg
        arg_data.append(arg_bytes)
        total_len += 4 + len(arg_bytes)
    
    # 构建消息
    msg = bytearray()
    
    # 4字节总长度
    msg.extend(struct.pack('!I', total_len))
    
    # 1字节命令类型
    msg.append(cmd_type)
    
    # 1字节参数数量
    msg.append(len(args))
    
    # 参数
    for arg_bytes in arg_data:
        msg.extend(struct.pack('!I', len(arg_bytes)))
        msg.extend(arg_bytes)
    
    return bytes(msg)

def decode_response(data):
    if len(data) < 4:
        return None
    
    # 读取总长度
    total_len = struct.unpack('!I', data[:4])[0]
    
    if len(data) < 4 + total_len:
        return None
    
    offset = 4
    
    # 读取命令类型
    cmd_type = data[offset]
    offset += 1
    
    # 读取参数数量
    arg_count = data[offset]
    offset += 1
    
    # 解析参数
    args = []
    for _ in range(arg_count):
        arg_len = struct.unpack('!I', data[offset:offset+4])[0]
        offset += 4
        arg = data[offset:offset+arg_len].decode('utf-8', errors='replace')
        args.append(arg)
        offset += arg_len
    
    return cmd_type, args

def send_command(sock, cmd_type, *args):
    msg = encode_command(cmd_type, *args)
    sock.sendall(msg)

def recv_response(sock):
    # 接收 4 字节长度
    length_data = sock.recv(4)
    if len(length_data) < 4:
        return None
    length = struct.unpack('!I', length_data)[0]
    
    # 接收完整消息
    msg = length_data
    remaining = length
    while len(msg) < 4 + remaining:
        chunk = sock.recv(4 + remaining - len(msg))
        if not chunk:
            return None
        msg += chunk
    
    return decode_response(msg)

def test_kv_server(host='localhost', port=6379):
    print(f'连接到 {host}:{port}...')
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    
    try:
        sock.connect((host, port))
        print('连接成功\n')
        
        # 测试 PING
        print('[1] PING')
        send_command(sock, CMD_PING)
        resp = recv_response(sock)
        print(f'响应: type={resp[0]}, args={resp[1]}')
        assert resp[0] == CMD_OK, 'PING 失败'
        print('✓ PING 通过\n')
        
        # 测试 SET（二进制安全值）
        print('[2] SET test_key test_value_with_\x00_null')
        test_value = 'test_value_with_\x00_null'
        send_command(sock, CMD_SET, 'test_key', test_value)
        resp = recv_response(sock)
        print(f'响应: type={resp[0]}')
        assert resp[0] == CMD_OK, 'SET 失败'
        print('✓ SET 通过\n')
        
        # 测试 GET（验证二进制安全）
        print('[3] GET test_key')
        send_command(sock, CMD_GET, 'test_key')
        resp = recv_response(sock)
        print(f'响应: type={resp[0]}, value={repr(resp[1][0])}')
        assert resp[0] == CMD_VALUE, 'GET 失败'
        assert test_value in resp[1][0], 'GET 值不匹配'
        print('✓ GET 通过（二进制安全）\n')
        
        # 测试 EXPIRE
        print('[4] EXPIRE test_key 5000')
        send_command(sock, CMD_EXPIRE, 'test_key', '5000')
        resp = recv_response(sock)
        print(f'响应: type={resp[0]}')
        assert resp[0] == CMD_OK, 'EXPIRE 失败'
        print('✓ EXPIRE 通过\n')
        
        # 测试 TTL
        print('[5] TTL test_key')
        send_command(sock, CMD_TTL, 'test_key')
        resp = recv_response(sock)
        print(f'响应: type={resp[0]}, ttl={resp[1]}')
        assert resp[0] == CMD_INTEGER, 'TTL 失败'
        print('✓ TTL 通过\n')
        
        # 测试 DEL
        print('[6] DEL test_key')
        send_command(sock, CMD_DEL, 'test_key')
        resp = recv_response(sock)
        print(f'响应: type={resp[0]}, deleted={resp[1]}')
        assert resp[0] == CMD_INTEGER, 'DEL 失败'
        print('✓ DEL 通过\n')
        
        # 测试 GET 不存在的键
        print('[7] GET test_key (不存在)')
        send_command(sock, CMD_GET, 'test_key')
        resp = recv_response(sock)
        print(f'响应: type={resp[0]}')
        assert resp[0] == CMD_NOTFOUND, 'GET 不存在键失败'
        print('✓ GET NOTFOUND 通过\n')
        
        print('=== 所有 KV 测试通过 ===')
        return True
        
    except Exception as e:
        print(f'错误: {e}')
        import traceback
        traceback.print_exc()
        return False
    finally:
        sock.close()

if __name__ == '__main__':
    port = 6379
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    
    success = test_kv_server(port=port)
    sys.exit(0 if success else 1)
