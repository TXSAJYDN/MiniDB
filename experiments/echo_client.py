#!/usr/bin/env python3
import socket
import sys
import time

def test_echo(host="localhost", port=8888):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    print(f"Connected to {host}:{port}")
    
    msg = "Hello, Echo!"
    sock.sendall(msg.encode())
    print(f"Sent: {msg}")
    
    data = sock.recv(1024)
    received = data.decode()
    print(f"Received: {received}")
    
    sock.close()
    return received == msg

def stress_test(host="localhost", port=8888, connections=5):
    print(f"
=== Stress Test: {connections} connections ===")
    socks = []
    for i in range(connections):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        socks.append(sock)
        msg = f"Message {i}"
        sock.sendall(msg.encode())
        print(f"Connection {i+1} sent")
    
    for i, sock in enumerate(socks):
        data = sock.recv(1024)
        print(f"Connection {i+1} received {len(data)} bytes")
        sock.close()

def partial_write_test(host="localhost", port=8888):
    print(f"
=== Partial Write Test ===")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
    sock.connect((host, port))
    
    large_data = b"X" * 100000
    sock.sendall(large_data)
    print(f"Sent {len(large_data)} bytes")
    
    received = b""
    while len(received) < len(large_data):
        chunk = sock.recv(4096)
        if not chunk:
            break
        received += chunk
    
    print(f"Received {len(received)} bytes")
    sock.close()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 echo_client.py <port> [test_type]")
        sys.exit(1)
    
    port = int(sys.argv[1])
    test_type = sys.argv[2] if len(sys.argv) > 2 else "echo"
    
    if test_type == "echo" or test_type == "all":
        test_echo(port=port)
    if test_type == "stress" or test_type == "all":
        time.sleep(0.5)
        stress_test(port=port)
    if test_type == "partial" or test_type == "all":
        time.sleep(0.5)
        partial_write_test(port=port)
