"""Run the final firmware daemon against HTTP/HTTPS platforms in a disposable
chroot and a private network/mount namespace. No hardware or flash is accessed.

Usage (root, qemu-user-binfmt installed):
  unshare --mount --net --fork python3 edge_platform_e2e.py ROOTFS OUTPUT
ROOTFS must be a disposable copy of an unpacked final firmware root filesystem.
"""
import json
import os
import re
import signal
import socket
import ssl
import stat
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

sys.dont_write_bytecode = True
from edge_ws_isolation_test import handshake
from edge_ws_transport_test import exact, read_frame, send_frame


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def field(number, value):
    if isinstance(value, int):
        return varint(number << 3) + varint(value)
    return varint((number << 3) | 2) + varint(len(value)) + value


def decode(data):
    result, offset = {}, 0

    def read_int():
        nonlocal offset
        value, shift = 0, 0
        while True:
            byte = data[offset]
            offset += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
            assert shift < 70

    while offset < len(data):
        tag = read_int()
        if tag & 7 == 0:
            value = read_int()
        elif tag & 7 == 2:
            length = read_int()
            value = data[offset:offset + length]
            offset += length
        elif tag & 7 in (1, 5):
            length = 8 if tag & 7 == 1 else 4
            value = data[offset:offset + length]
            offset += length
        else:
            raise AssertionError(f"invalid protobuf wire type: {tag}")
        result[tag >> 3] = value
    assert offset == len(data)
    return result


class Connection:
    def __init__(self, platform, sock, epoch):
        self.platform, self.sock, self.epoch = platform, sock, epoch
        self.lock = threading.Lock()
        self.node = b"N" * 16
        self.ready = False
        self.closed = False
        self.heartbeats = 0
        self.output = bytearray()
        self.terminal_opened = False
        self.terminal_closed = False
        self.terminal = uuid.uuid4().bytes
        self.input_sequence = 0
        self.ack_output = True

    def send(self, tag, payload, origin=None):
        wire = (field(1, 6) + field(2, uuid.uuid4().bytes) + field(3, self.node) +
                field(4, origin or self.platform.id) + field(5, self.epoch) +
                field(tag, payload))
        with self.lock:
            send_frame(self.sock, 0x82, wire)

    def run(self):
        try:
            handshake(self.sock)
            while True:
                opcode, data = read_frame(self.sock)
                if opcode == 0x88:
                    with self.lock:
                        send_frame(self.sock, 0x88, data)
                    break
                if opcode == 0x89:
                    with self.lock:
                        send_frame(self.sock, 0x8A, data)
                    continue
                assert opcode == 0x82, opcode
                envelope = decode(data)
                assert envelope[4] == self.platform.id, "cross-platform envelope"
                if self.platform.observer:
                    self.platform.observer(self, envelope)
                if 20 in envelope:
                    hello = decode(envelope[20])
                    assert hello[3] == b"0.3.80", hello.get(3)
                    self.send(21, field(1, self.node) + field(2, self.epoch) +
                              field(3, 6) + field(4, 1) + field(5, 16384))
                    self.ready = True
                elif 24 in envelope:
                    self.heartbeats += 1
                elif 76 in envelope:
                    assert decode(envelope[76])[1] == self.terminal
                    self.terminal_opened = True
                elif 71 in envelope:
                    terminal = decode(envelope[71])
                    assert terminal[1] == self.terminal, "cross-platform terminal output"
                    self.output.extend(terminal[2])
                    if self.ack_output:
                        self.send(77, field(1, self.terminal) + field(2, terminal[3]))
                elif 73 in envelope:
                    self.terminal_closed = True
        except (OSError, AssertionError, IndexError) as error:
            if not self.platform.stopping and not isinstance(error, OSError):
                # EOF is expected for injected crashes; all other assertions fail E2E.
                if str(error) != "unexpected EOF":
                    self.platform.errors.append(repr(error))
        finally:
            self.closed = True
            self.sock.close()

    def open_terminal(self):
        self.send(70, field(1, self.terminal) + field(3, 80) + field(4, 24))

    def command(self, command):
        self.input_sequence += 1
        self.send(71, field(1, self.terminal) + field(2, command.encode()) +
                  field(3, self.input_sequence))


class Platform:
    def __init__(self, number, tls=None):
        self.id = uuid.UUID(f"00000000-0000-7000-8000-{number:012d}").bytes
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.tls = tls
        self.connections, self.errors = [], []
        self.stopping = False
        self.available = True
        self.observer = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stopping:
            try:
                sock, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            sock.settimeout(120)
            if not self.available:
                sock.close()
                continue
            if self.tls:
                sock = self.tls.wrap_socket(sock, server_side=True)
            connection = Connection(self, sock, len(self.connections) + 1)
            self.connections.append(connection)
            threading.Thread(target=connection.run, daemon=True).start()

    @property
    def current(self):
        return self.connections[-1] if self.connections else None

    def stop(self):
        self.stopping = True
        self.listener.close()
        for connection in self.connections:
            try:
                connection.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def wait_for(predicate, label, timeout=25, check=lambda: None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        check()
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"timeout: {label}")


def worker_for_port(parent, port):
    inodes = set()
    for line in Path("/proc/net/tcp").read_text().splitlines()[1:]:
        parts = line.split()
        if int(parts[2].split(":")[1], 16) == port:
            inodes.add("socket:[" + parts[9] + "]")
    children = Path(f"/proc/{parent}/task/{parent}/children").read_text().split()
    for child in children:
        directory = Path(f"/proc/{child}")
        try:
            if directory.joinpath("comm").read_text().strip() != "edgenode-ws":
                continue
            for descriptor in directory.joinpath("fd").iterdir():
                if os.readlink(descriptor) in inodes:
                    return int(child)
        except FileNotFoundError:
            continue
    raise AssertionError("platform worker not found")


def main():
    root, output = map(lambda value: Path(value).resolve(), sys.argv[1:3])
    assert root != Path("/") and (root / "usr/sbin/edgenode").is_file()
    assert os.readlink("/proc/self/ns/net") != os.readlink("/proc/1/ns/net"), "private netns required"
    assert os.readlink("/proc/self/ns/mnt") != os.readlink("/proc/1/ns/mnt"), "private mount namespace required"
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
    mounted = []
    platforms = []
    daemon = None
    logger = None
    report = {"cases": [], "binary_sha256": __import__("hashlib").sha256(
        (root / "usr/sbin/edgenode").read_bytes()).hexdigest()}
    try:
        for directory in ("dev", "dev/pts", "proc", "tmp", "etc/config", "etc/ssl/certs"):
            (root / directory).mkdir(parents=True, exist_ok=True)
        for name, major, minor in (("null", 1, 3), ("zero", 1, 5), ("random", 1, 8), ("urandom", 1, 9)):
            path = root / "dev" / name
            if not path.exists():
                os.mknod(path, stat.S_IFCHR | 0o666, os.makedev(major, minor))
        for kind, destination, options in (("proc", "proc", ""), ("devpts", "dev/pts", "newinstance,ptmxmode=0666,mode=0620")):
            command = ["mount", "-t", kind]
            if options:
                command += ["-o", options]
            subprocess.run(command + [kind, str(root / destination)], check=True)
            mounted.append(root / destination)
        ptmx = root / "dev/ptmx"
        if ptmx.is_symlink() or ptmx.exists():
            ptmx.unlink()
        ptmx.symlink_to("pts/ptmx")
        # A flashed router creates this file during board first-boot setup.
        # The disposable chroot has only loopback and must not use host networking.
        (root / "etc/config/network").write_text(
            "config interface 'loopback'\n option device 'lo'\n option proto 'static'\n"
            " option ipaddr '127.0.0.1'\n option netmask '255.0.0.0'\n")
        logger = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        logger.settimeout(0.2)
        log_socket = root / "dev/log"
        if log_socket.exists():
            log_socket.unlink()
        original_cwd = Path.cwd()
        os.chdir(root / "dev")
        logger.bind("log")  # Avoid sockaddr_un's limit on a long artifact directory.
        os.chdir(original_cwd)

        def capture_syslog():
            with (output / "syslog.log").open("ab") as stream:
                while True:
                    try:
                        stream.write(logger.recv(8192) + b"\n")
                        stream.flush()
                    except socket.timeout:
                        continue
                    except OSError:
                        return
        threading.Thread(target=capture_syslog, daemon=True).start()
        cert, key = output / "test-ca.pem", output / "test-ca.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
                        "-out", str(cert), "-keyout", str(key)], check=True, capture_output=True)
        key.chmod(0o600)
        with (root / "etc/ssl/certs/ca-certificates.crt").open("ab") as trust:
            trust.write(b"\n" + cert.read_bytes())
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        a, b = Platform(1), Platform(2, tls)
        platforms = [a, b]
        config = "config node 'node'\n option imei '490154203237518'\n option model 'e2e'\n"
        for index, platform in enumerate(platforms):
            config += (f"config platform 'p{index}'\n option id '{uuid.UUID(bytes=platform.id)}'\n"
                       f" option url '{'https' if platform.tls else 'http'}://127.0.0.1:{platform.port}'\n"
                       f" option enabled '1'\n option priority '{index}'\n option reconnect_interval_sec '1'\n")
        (root / "etc/config/edgenode").write_text(config)
        # All modifications above are to this disposable test clone, never the image.
        with (output / "daemon.log").open("wb") as log:
            daemon = subprocess.Popen(["chroot", str(root), "/usr/sbin/edgenode"],
                                      stdout=log, stderr=subprocess.STDOUT)

            def healthy():
                assert daemon.poll() is None, "shared main process exited"
                assert not a.errors and not b.errors, (a.errors, b.errors)

            wait_for(lambda: a.current and b.current and a.current.ready and b.current.ready,
                     "two enrolled platforms", check=healthy)
            if "--acquisition" in sys.argv[3:]:
                from edge_platform_acquisition_e2e import run_acquisition
                run_acquisition(platforms, daemon, report, healthy)
                return
            stable = b.current
            a.current.open_terminal()
            stable.open_terminal()
            wait_for(lambda: a.current.terminal_opened and stable.terminal_opened,
                     "concurrent terminals", check=healthy)
            stable.command("stty -echo\n")
            time.sleep(0.5)
            stable.command("isolation_token=kept; printf 'IDENTITY:%s:%s\\n' $isolation_token $$\n")
            wait_for(lambda: re.search(rb"IDENTITY:kept:(\d+)\r?\n", stable.output),
                     "persistent shell identity", check=healthy)
            shell_pid = re.search(rb"IDENTITY:kept:(\d+)\r?\n", stable.output)[1]
            report["cases"].append("HTTP and HTTPS enrolled; concurrent terminals")

            def probe():
                healthy()
                assert b.current is stable and not stable.closed and not stable.terminal_closed
                marker = uuid.uuid4().hex
                stable.command(f"printf 'PROBE:%s:%s:%s\\n' $isolation_token $$ {marker}\n")
                expected = b"PROBE:kept:" + shell_pid + b":" + marker.encode()
                wait_for(lambda: expected in stable.output, "healthy terminal reply", timeout=3, check=healthy)

            for fault in ("frozen worker", "worker crash", "text frame", "wrong platform identity", "empty binary"):
                previous = a.current
                if fault in ("frozen worker", "worker crash"):
                    os.kill(worker_for_port(daemon.pid, a.port), signal.SIGSTOP if fault == "frozen worker" else signal.SIGKILL)
                elif fault == "text frame":
                    with previous.lock:
                        send_frame(previous.sock, 0x81, b"invalid application text")
                elif fault == "empty binary":
                    with previous.lock:
                        send_frame(previous.sock, 0x82, b"")
                else:
                    previous.send(70, field(1, stable.terminal) + field(3, 80) + field(4, 24), origin=b.id)
                deadline = time.monotonic() + 40
                while time.monotonic() < deadline:
                    probe()
                    if a.current is not previous and a.current.ready:
                        break
                    time.sleep(0.25)
                else:
                    raise AssertionError(f"failed to recover A after {fault}")
                probe()
                report["cases"].append(f"{fault}: only A reconnected; B shell and session preserved")
            assert len(b.connections) == 1
            assert stable.heartbeats >= 10
            report.update(passed=True, healthy_connections=len(b.connections),
                          fault_connections=len(a.connections), healthy_heartbeats=stable.heartbeats)
    finally:
        if daemon and daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=10)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
        for platform in platforms:
            platform.stop()
        if logger:
            logger.close()
        for path in reversed(mounted):
            subprocess.run(["umount", str(path)], check=True)
        (output / "e2e.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
