"""Acquisition extension for edge_platform_e2e.py --acquisition.

One simulated Modbus TCP instrument serves two slaves through shared physical
I/O. The final firmware performs all scheduling, parsing, spooling and uploads.
Both platform directions are faulted; healthy samples must reach their own
platform within 3 seconds, with acquisition/upload gaps below 2.5 seconds.
"""
import hashlib
import os
import re
import signal
import socket
import struct
import threading
import time
import uuid

from edge_platform_e2e import decode, field, wait_for, worker_for_port
from edge_ws_transport_test import exact


class Instrument:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.samples = {1: [], 2: []}
        self.errors = []
        self.stopping = False
        self.sockets = []
        self.lock = threading.Lock()
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while not self.stopping:
            try:
                sock, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            sock.settimeout(120)
            self.sockets.append(sock)
            threading.Thread(target=self.serve, args=(sock,), daemon=True).start()

    def serve(self, sock):
        try:
            while not self.stopping:
                header = exact(sock, 7)
                transaction, protocol, length, slave = struct.unpack('>HHHB', header)
                assert protocol == 0 and length == 6 and slave in (1, 2)
                request = exact(sock, length - 1)
                assert request == struct.pack('>BHH', 3, slave * 10, 1), request.hex()
                with self.lock:
                    sequence = len(self.samples[slave]) + 1
                    value = slave * 10000 + sequence
                    response = struct.pack('>HHHBBBH', transaction, 0, 5, slave, 3, 2, value)
                    self.samples[slave].append(dict(sequence=sequence, value=value,
                                                   time=time.monotonic(), raw=response.hex()))
                sock.sendall(response)
        except (OSError, AssertionError) as error:
            if not self.stopping and isinstance(error, AssertionError) and str(error) != 'unexpected EOF':
                self.errors.append(repr(error))
        finally:
            sock.close()

    def stop(self):
        self.stopping = True
        self.listener.close()
        for sock in self.sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Acquisition:
    def __init__(self, platform, instrument, slave):
        self.platform, self.instrument, self.slave = platform, instrument, slave
        # Identical object IDs and revisions deliberately exercise platform scoping.
        self.endpoint = uuid.UUID('10000000-0000-7000-8000-000000000001').bytes
        self.device = uuid.UUID('20000000-0000-7000-8000-000000000001').bytes
        self.applied = False
        self.records = {}
        self.values = {}
        self.terminal_latencies = []
        endpoint = (field(1, self.endpoint) + field(2, b'shared instrument') + field(3, 1) +
                    field(4, 2) + field(5, 2) + field(6, b'127.0.0.1') +
                    field(7, instrument.port) + field(9, 1) + field(10, b'lo'))
        device = (field(1, self.device) + field(2, self.endpoint) + field(3, b'PLC') +
                  field(5, 2) + field(7, 1000) + field(8, 1) + field(9, 10) +
                  field(10, slave) + field(11, b'TCP') + field(24, 1))
        point = (field(1, self.device) + field(2, b'30000000-0000-7000-8000-000000000001') +
                 field(3, b'counter') + field(5, b'HOLDING_REGISTER') + field(6, b'UINT16') +
                 field(7, b'BIG_ENDIAN') + field(8, slave * 10) + field(9, 1) +
                 bytes([81]) + struct.pack('<d', 1.0))
        self.items, hashes = [], []
        for index, payload in enumerate((endpoint, device, point)):
            prefix = field(1, 1) + (field(2, index) if index else b'') + field(3, index + 1)
            body = field(10 + index, payload)
            digest = hashlib.sha256(prefix + body).digest()
            self.items.append(prefix + field(4, digest) + body)
            hashes.append(digest)
        self.digest = hashlib.sha256(b''.join(hashes)).digest()
        platform.observer = self.receive

    def configure(self):
        connection = self.platform.current
        connection.send(30, field(1, 1) + field(2, 3) + field(3, self.digest))
        for item in self.items:
            connection.send(31, item)
        connection.send(32, field(1, 1) + field(2, self.digest))

    def receive(self, connection, envelope):
        if 34 in envelope:
            raise AssertionError(('configuration rejected', decode(envelope[34])))
        if 33 in envelope:
            applied = decode(envelope[33])
            assert applied[1] == 1 and applied[2] == self.digest
            assert applied[3] == 1 and applied[4] == 1
            self.applied = True
        if 40 not in envelope:
            return
        record = decode(decode(envelope[40])[1])
        assert record[2] == self.device and record[3] == self.endpoint
        assert record[4] == 2
        if 21 in record:
            assert record[21] == self.digest, 'cross-platform configuration digest'
        raw = record[14]
        assert len(raw) == 11 and raw[6:9] == bytes([self.slave, 3, 2]), raw.hex()
        value = struct.unpack('>H', raw[-2:])[0]
        sequence = value - self.slave * 10000
        assert sequence > 0, 'cross-platform sample'
        with self.instrument.lock:
            sample = self.instrument.samples[self.slave][sequence - 1].copy()
        assert sample['raw'] == raw.hex(), 'fabricated or altered telemetry'
        if 9 in record:
            scalar = decode(decode(record[9])[4])
            actual = (struct.unpack('<d', scalar[5])[0] if 5 in scalar else
                      scalar[4] if 4 in scalar else scalar[3] >> 1)
            assert actual == value, ('parsed value mismatch', actual, value)
        if record[1] not in self.records:
            received = time.monotonic()
            self.records[record[1]] = dict(sequence=sequence, time=received)
            self.values.setdefault(sequence, dict(sample, received=received))
        connection.send(41, field(1, record[1]))

    def terminal(self, check):
        connection = self.platform.current
        if not connection.terminal_opened:
            connection.open_terminal()
        wait_for(lambda: connection.terminal_opened, 'terminal opened', check=check)
        connection.command('stty -echo\n')
        time.sleep(0.3)
        connection.command("isolation_token=kept; printf 'IDENTITY:%s:%s\\n' $isolation_token $$\n")
        wait_for(lambda: re.search(rb'IDENTITY:kept:(\d+)\r?\n', connection.output),
                 'shell identity', check=check)
        pid = re.search(rb'IDENTITY:kept:(\d+)\r?\n', connection.output)[1]
        return connection, pid

    def probe(self, stable, pid, check):
        assert self.platform.current is stable and not stable.closed and not stable.terminal_closed
        marker = uuid.uuid4().hex
        before = time.monotonic()
        stable.command(f"printf 'PROBE:%s:%s:%s\\n' $isolation_token $$ {marker}\n")
        expected = b'PROBE:kept:' + pid + b':' + marker.encode()
        wait_for(lambda: expected in stable.output, 'healthy terminal', timeout=3, check=check)
        self.terminal_latencies.append(time.monotonic() - before)

    def continuity(self, start):
        now = time.monotonic()
        with self.instrument.lock:
            samples = [s.copy() for s in self.instrument.samples[self.slave] if s['time'] >= start]
        if now - start > 2.5:
            assert samples and now - samples[-1]['time'] < 2.5, 'acquisition stalled'
            received = list(self.values.values())
            assert received and now - max(s['received'] for s in received) < 2.5, 'upload stalled'
        for sample in samples:
            if now - sample['time'] >= 3:
                assert sample['sequence'] in self.values, ('missing healthy sample', sample)
                assert self.values[sample['sequence']]['received'] - sample['time'] < 3, 'delayed sample'
        for left, right in zip(samples, samples[1:]):
            assert right['time'] - left['time'] < 2.5, 'acquisition gap'

    def metrics(self, start, end):
        values = sorted((v for v in list(self.values.values()) if start <= v['time'] <= end),
                        key=lambda v: v['time'])
        assert len(values) >= 3, 'insufficient actual telemetry'
        times = sorted(v['received'] for v in values)
        return dict(samples=len(values), max_acquisition_gap_sec=max(
            b['time'] - a['time'] for a, b in zip(values, values[1:])),
            max_upload_gap_sec=max(b-a for a, b in zip(times, times[1:])),
            max_upload_latency_sec=max(v['received'] - v['time'] for v in values),
            max_terminal_latency_sec=max(self.terminal_latencies))


def run_acquisition(platforms, daemon, report, healthy):
    instrument = Instrument()
    report.update(test='dual-platform acquisition/upload/terminal fault isolation',
                  interval_ms=1000, thresholds=dict(gap_sec=2.5, upload_latency_sec=3, terminal_sec=3),
                  directions=[])
    try:
        clients = [Acquisition(p, instrument, i + 1) for i, p in enumerate(platforms)]

        def check():
            healthy()
            assert not instrument.errors, instrument.errors

        for client in clients:
            client.configure()
        wait_for(lambda: all(c.applied and len(c.values) >= 3 for c in clients),
                 'both configurations applied and real samples uploaded', check=check)
        for faulty, good in ((clients[0], clients[1]), (clients[1], clients[0])):
            stable, pid = good.terminal(check)
            faulty.terminal(check)
            connection_count = len(good.platform.connections)
            start = time.monotonic()

            def monitor(duration=0, until=None, timeout=330):
                deadline = time.monotonic() + (timeout if until else duration)
                while time.monotonic() < deadline:
                    check()
                    good.continuity(start)
                    good.probe(stable, pid, check)
                    if until and until():
                        return
                    time.sleep(0.2)
                assert until is None, 'faulty platform did not recover'

            monitor(8)
            baseline_end = time.monotonic()
            baseline = good.metrics(start, baseline_end)
            phases = []
            # Run rapid flaps first. The production retry policy doubles up to
            # 300 seconds and only resets after five minutes of stable service.
            # Respect that policy without needlessly accumulating retries before
            # this scenario; healthy-side continuity deadlines remain unchanged.
            for name in ('reconnect storm', 'frozen worker', 'disconnected 20 seconds'):
                phase_start = time.monotonic()
                previous = faulty.platform.current
                if name == 'disconnected 20 seconds':
                    faulty.platform.available = False
                    previous.sock.shutdown(socket.SHUT_RDWR)
                    monitor(20)
                    faulty.platform.available = True
                    monitor(until=lambda: faulty.platform.current is not previous and faulty.platform.current.ready)
                elif name == 'frozen worker':
                    os.kill(worker_for_port(daemon.pid, faulty.platform.port), signal.SIGSTOP)
                    monitor(until=lambda: faulty.platform.current is not previous and faulty.platform.current.ready)
                else:
                    for _ in range(4):
                        previous = faulty.platform.current
                        previous.sock.shutdown(socket.SHUT_RDWR)
                        monitor(until=lambda: faulty.platform.current is not previous and faulty.platform.current.ready)
                monitor(4)
                phase_end = time.monotonic()
                metrics = good.metrics(phase_start, phase_end)
                assert metrics['max_upload_gap_sec'] < 2.5, metrics
                phases.append(dict(fault=name, duration_sec=phase_end-phase_start, **metrics))
                report['cases'].append(f'fault platform {faulty.slave}: {name}; healthy acquisition/upload/terminal preserved')
            end = time.monotonic()
            monitor(4)
            good.continuity(start)
            assert len(good.platform.connections) == connection_count
            # Fault recovery must resume that platform's own real data as well.
            wait_for(lambda: any(v['time'] > end for v in list(faulty.values.values())),
                     'faulty platform resumes own telemetry', check=check)
            report['directions'].append(dict(fault_platform=faulty.slave, healthy_platform=good.slave,
                                            baseline=baseline, phases=phases, healthy_reconnections=0))
        report.update(passed=True, platform_samples={str(c.slave): len(c.values) for c in clients},
                      platform_connections={str(c.slave): len(c.platform.connections) for c in clients})
    finally:
        report['instrument_samples'] = instrument.samples
        report['uploaded_samples'] = {str(c.slave): list(c.values.values()) for c in clients}
        instrument.stop()
