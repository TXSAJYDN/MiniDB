#!/usr/bin/env python3
"""Deterministic Linux TCP echo client used by the Stage B experiment."""

import argparse
import socket
import struct
import sys
import threading
import time


def connect(host, port, timeout=10.0, receive_buffer=None):
    sock = socket.create_connection((host, port), timeout=timeout)
    if receive_buffer is not None:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
    return sock


def recv_exact(sock, length):
    chunks = []
    remaining = length
    while remaining:
        chunk = sock.recv(min(remaining, 65536))
        if not chunk:
            raise RuntimeError(
                f"unexpected EOF with {remaining} of {length} bytes missing")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def ensure_eof(sock, timeout=5.0):
    sock.settimeout(timeout)
    trailing = sock.recv(1)
    if trailing:
        raise RuntimeError("server sent bytes after expected payload")
    return True


def payload_for(label, length):
    seed = bytearray(label.encode("ascii", "strict"))
    if not seed:
        seed = bytearray(b"x")
    data = bytearray(length)
    for index in range(length):
        data[index] = seed[index % len(seed)] ^ ((index * 131) & 0xFF)
    return bytes(data)


def test_basic(host, port):
    sock = connect(host, port)
    try:
        messages = [b"Hello, Echo!", b"", b"\x00binary\xffpayload", b"line\r\n"]
        for message in messages:
            sock.sendall(message)
            if message:
                received = recv_exact(sock, len(message))
                if received != message:
                    raise RuntimeError("basic echo payload mismatch")
    finally:
        sock.close()
    print("PASS basic")


def test_fragmented(host, port):
    data = payload_for("fragmented", 257)
    sock = connect(host, port)
    try:
        for index, value in enumerate(data):
            sock.sendall(bytes([value]))
            if index % 31 == 0:
                time.sleep(0.001)
        received = recv_exact(sock, len(data))
        if received != data:
            raise RuntimeError("fragmented echo payload mismatch")
    finally:
        sock.close()
    print("PASS fragmented")


def test_stress(host, port, connections=16):
    sockets = []
    expected = []
    try:
        for index in range(connections):
            data = payload_for(f"stress-{index}", 8192)
            sock = connect(host, port)
            sock.sendall(data)
            sock.shutdown(socket.SHUT_WR)
            sockets.append(sock)
            expected.append(data)
        for index, sock in enumerate(sockets):
            received = recv_exact(sock, len(expected[index]))
            if received != expected[index]:
                raise RuntimeError(f"stress connection {index} payload mismatch")
    finally:
        for sock in sockets:
            sock.close()
    print(f"PASS stress connections={connections}")


def test_large_slow(host, port, length=512 * 1024):
    data = payload_for("large-slow", length)
    sock = connect(host, port, receive_buffer=4096)
    try:
        view = memoryview(data)
        offset = 0
        while offset < len(view):
            chunk = view[offset:offset + 8192]
            sock.sendall(chunk)
            offset += len(chunk)
        received = []
        remaining = length
        while remaining:
            chunk = sock.recv(min(remaining, 16384))
            if not chunk:
                raise RuntimeError(
                    f"slow-read echo ended with {remaining} bytes missing")
            received.append(chunk)
            remaining -= len(chunk)
            if remaining % (64 * 1024) == 0:
                time.sleep(0.002)
        actual = b"".join(received)
        if actual != data:
            raise RuntimeError("slow-read echo payload mismatch")
    finally:
        sock.close()
    print(f"PASS large_slow bytes={length}")


def test_half_close(host, port):
    data = payload_for("half-close", 128 * 1024)
    sock = connect(host, port)
    try:
        sock.sendall(data)
        sock.shutdown(socket.SHUT_WR)
        received = recv_exact(sock, len(data))
        if received != data:
            raise RuntimeError("half-close echo payload mismatch")
        ensure_eof(sock)
    finally:
        sock.close()
    print(f"PASS half_close bytes={len(data)}")


def test_multi_client(host, port, clients=8, length=256 * 1024):
    errors = []
    barrier = threading.Barrier(clients)

    def worker(index):
        try:
            data = payload_for(f"fair-{index}", length)
            sock = connect(host, port)
            try:
                barrier.wait(timeout=10)
                sock.sendall(data)
                received = recv_exact(sock, len(data))
                if received != data:
                    raise RuntimeError("multi-client payload mismatch")
            finally:
                sock.close()
        except Exception as error:  # pylint: disable=broad-except
            errors.append(f"client {index}: {error}")

    threads = [threading.Thread(target=worker, args=(index,))
               for index in range(clients)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=60)
    for thread in threads:
        if thread.is_alive():
            raise RuntimeError("multi-client worker timed out")
    if errors:
        raise RuntimeError("; ".join(errors))
    print(f"PASS multi_client clients={clients} bytes_each={length}")


def test_abrupt_close(host, port):
    data = payload_for("abrupt", 256 * 1024)
    sock = connect(host, port)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        struct.pack("ii", 1, 0))
        sock.sendall(data)
    finally:
        sock.close()
    # A failed write or reset connection must not stop the server.
    probe = connect(host, port)
    try:
        probe.sendall(b"still-alive")
        if recv_exact(probe, len(b"still-alive")) != b"still-alive":
            raise RuntimeError("server did not recover after abrupt close")
    finally:
        probe.close()
    print("PASS abrupt_close")


TESTS = {
    "basic": test_basic,
    "fragmented": test_fragmented,
    "stress": test_stress,
    "large_slow": test_large_slow,
    "half_close": test_half_close,
    "multi_client": test_multi_client,
    "abrupt_close": test_abrupt_close,
}

BLOCKING_TESTS = [
    "basic",
    "fragmented",
    "stress",
    "large_slow",
    "half_close",
    "abrupt_close",
]


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--test",
                        choices=sorted([*TESTS, "all", "blocking_all"]),
                        default="all")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.test == "all":
        names = list(TESTS)
    elif args.test == "blocking_all":
        names = BLOCKING_TESTS
    else:
        names = [args.test]
    try:
        for name in names:
            TESTS[name](args.host, args.port)
    except (OSError, RuntimeError, TimeoutError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
