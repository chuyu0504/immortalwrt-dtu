"""通过真实 TCP 零窗口验证未确认回执的重连重发。"""
import base64
import hashlib
import socket
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from edge_ws_transport_test import exact, read_frame, send_frame


def handshake(sock):
    request = bytearray()
    while not request.endswith(b"\r\n\r\n"):
        request.extend(exact(sock, 1))
        assert len(request) < 8192
    headers = dict(line.split(":", 1) for line in request.decode().split("\r\n")[1:]
                   if ":" in line)
    headers = {key.lower(): value.strip() for key, value in headers.items()}
    accept = base64.b64encode(hashlib.sha1(
        (headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
    ).digest()).decode()
    sock.sendall(("HTTP/1.1 101 Switching Protocols\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())


with socket.socket() as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    listener.bind(("127.0.0.1", 0))
    listener.listen(2)
    listener.settimeout(20)
    process = subprocess.Popen(
        [sys.argv[1], f"ws://127.0.0.1:{listener.getsockname()[1]}/edge/connect"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        first, _ = listener.accept()
        first.settimeout(5)
        handshake(first)
        send_frame(first, 0x82, b"fill")
        started = time.monotonic()
        second, _ = listener.accept()
        elapsed = time.monotonic() - started
        assert 8 <= elapsed <= 18, f"recovery took {elapsed:.3f}s"
        second.settimeout(8)
        handshake(second)
        opcode, payload = read_frame(second)
        assert opcode == 0x82 and payload == b"command-result", (opcode, payload)
        send_frame(second, 0x82, b"ack")
        opcode, payload = read_frame(second)
        assert opcode == 0x88
        send_frame(second, 0x88, payload)
        second.close()
        first.close()
        stdout, stderr = process.communicate(timeout=8)
        assert process.returncode == 0, stderr.decode()
        print(f"pending result replayed and acknowledged after {elapsed:.3f}s")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
