"""Two platforms in one app: connect stalls, process faults and hostile traffic."""
import concurrent.futures
import base64
import hashlib
import os
import socket
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

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


def serve_good(listener, target, tls=None):
    sock = listener.accept()[0]
    if tls:
        sock = tls.wrap_socket(sock, server_side=True)
    with sock:
        sock.settimeout(10)
        handshake(sock)
        for _ in range(target):
            opcode, payload = read_frame(sock)
            assert opcode == 0x82
            send_frame(sock, 0x82, payload)
        # Let the application consume the last reply before closing the server.
        while sock.recv(4096):
            pass


def serve_bad(listener, mode, tls=None):
    if mode == "stall":
        return
    with listener.accept()[0] as sock:
        sock.settimeout(20)
        if mode == "tls-reject":
            try:
                tls.wrap_socket(sock, server_side=True)
            except ssl.SSLError:
                return
            raise AssertionError("untrusted certificate unexpectedly accepted")
        handshake(sock)
        try:
            if mode == "flood":
                for _ in range(20000):
                    send_frame(sock, 0x82, b"x" * 128)
            elif mode == "oversize":
                send_frame(sock, 0x82, b"x" * 17000)
                opcode, payload = read_frame(sock)
                assert opcode == 0x88
                send_frame(sock, 0x88, payload)
            while sock.recv(4096):
                pass
        except (ConnectionResetError, BrokenPipeError):
            pass


def scenario(executable, mode, tls=None, ca=None):
    with socket.socket() as good, socket.socket() as bad:
        for listener in (good, bad):
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(12)
        scheme = "wss" if tls and mode != "tls-reject" else "ws"
        good_url = f"{scheme}://127.0.0.1:{good.getsockname()[1]}/edge/connect"
        bad_url = ("ws://stall.invalid/edge/connect" if mode == "stall" else
                   f"{'wss' if mode == 'tls-reject' else 'ws'}://127.0.0.1:{bad.getsockname()[1]}/edge/connect")
        target = 120 if mode == "watchdog" else 30
        environment = os.environ.copy()
        if ca:
            environment["EDGE_WS_TEST_CA"] = str(ca)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(serve_good, good, target, tls if scheme == "wss" else None),
                       pool.submit(serve_bad, bad, mode, tls)]
            result = subprocess.run([executable, good_url, bad_url, mode],
                                    capture_output=True, timeout=20, env=environment)
            assert result.returncode == 0, result.stderr.decode()
            for future in futures:
                future.result(timeout=12)
            print(scheme, result.stdout.decode().strip())


if __name__ == "__main__":
    for mode in ("stall", "freeze", "crash", "queue", "destroy", "flood", "oversize", "watchdog"):
        scenario(sys.argv[1], mode)
    with tempfile.TemporaryDirectory() as directory:
        cert = Path(directory) / "cert.pem"
        key = Path(directory) / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=IP:127.0.0.1", "-out", str(cert),
                        "-keyout", str(key)], check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        scenario(sys.argv[1], "stall", context, cert)
        # Without the test CA, production verification must reject this cert.
        scenario(sys.argv[1], "tls-reject", context)
