#!/usr/bin/env python3
"""Self-contained local demonstration; no dependency on tests/ or third-party packages."""

import argparse
import collections
import errno
import http.client
import http.server
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
TIMEOUT = 5
STOP = threading.Event()


class DemoError(RuntimeError):
    pass


class DemoStopped(Exception):
    pass


def check(name, expected, actual, passed):
    print(f"[{'PASS' if passed else 'FAIL'}] {name}: expected={expected}; actual={actual}", flush=True)
    if not passed:
        raise DemoError(name)


def environment():
    return {key: value for key, value in os.environ.items() if not key.startswith('APIGATE_')}


def stop_process(process, group=False):
    if process.poll() is None:
        if group:
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if group:
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait(timeout=5)


def build():
    env = environment()
    env.update(CMAKE_BUILD_PARALLEL_LEVEL='1', VCPKG_MAX_CONCURRENCY='1')
    process = subprocess.Popen(['bash', str(ROOT / 'scripts/build.sh'), 'linux-debug'],
                               cwd=ROOT, env=env, start_new_session=True)
    try:
        deadline = time.monotonic() + 3600
        while not STOP.is_set() and time.monotonic() < deadline:
            try:
                code = process.wait(timeout=0.2)
                if code:
                    raise DemoError(f'build failed (exit {code})')
                return ROOT / 'build/linux-debug/bin/api-gate'
            except subprocess.TimeoutExpired:
                continue
        if STOP.is_set():
            raise DemoStopped()
        raise DemoError('build exceeded 3600 seconds')
    finally:
        stop_process(process, group=True)


class Backend(http.server.HTTPServer):
    def __init__(self, marker):
        super().__init__(('127.0.0.1', 0), Handler)
        self.marker = marker
        self.prefix_sent = threading.Event()
        self.release_tail = threading.Event()
        self.errors = []
        self.stopping = threading.Event()
        self.active_lock = threading.Lock()
        self.active = None
        self.timeout = 0.2
        self.thread = threading.Thread(target=self.run)
        try:
            self.thread.start()
        except BaseException:
            self.server_close()
            raise

    def run(self):
        while not self.stopping.is_set():
            self.handle_request()

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(TIMEOUT)
        with self.active_lock:
            self.active = connection
            if self.stopping.is_set():
                self.cancel_active()
        return connection, address

    def close_request(self, connection):
        with self.active_lock:
            super().close_request(connection)
            if self.active is connection:
                self.active = None

    def cancel_active(self):
        # Caller holds active_lock. Interrupt any blocked body/header read or write.
        if self.active is not None:
            try:
                self.active.shutdown(socket.SHUT_RDWR)
            except OSError as error:
                if error.errno not in (errno.ENOTCONN, errno.EBADF):
                    raise

    def handle_error(self, request, client_address):
        if not self.stopping.is_set():
            self.errors.append('backend handler failed')

    def close(self):
        self.stopping.set()
        self.release_tail.set()
        with self.active_lock:
            self.cancel_active()
        self.thread.join(timeout=TIMEOUT + 1)
        self.server_close()
        if self.thread.is_alive():
            raise DemoError('backend thread did not stop')


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def do_GET(self):
        size = int(self.headers.get('Content-Length', '0'))
        if not 0 <= size <= 65536:
            raise DemoError('demo backend body limit')
        body = self.rfile.read(size)
        if self.path == '/stream' and self.command == 'GET':
            self.send_response(200)
            self.send_header('Content-Length', '11')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'first-')
            self.wfile.flush()
            self.server.prefix_sent.set()
            if not self.server.release_tail.wait(TIMEOUT):
                raise DemoError('stream tail was not released in time')
            self.wfile.write(b'last!')
        else:
            response = json.dumps({'backend': self.server.marker, 'method': self.command,
                                   'target': self.path, 'body': body.decode('utf-8'),
                                   'request_ids': self.headers.get_all('X-Request-ID', [])}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(response)))
            self.send_header('X-Request-ID', 'untrusted-backend')
            self.send_header('Connection', 'close')
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(response)
        self.wfile.flush()
        self.close_connection = True

    do_POST = do_GET
    do_HEAD = do_GET


class Gateway:
    def __init__(self, binary, directory, backends):
        self.process = None
        self.reader = None
        self.condition = threading.Condition()
        self.events = collections.deque(maxlen=1024)
        self.ended = False
        self.log = directory / 'gateway.jsonl'
        self.err = directory / 'gateway.stderr'
        self.output = self.log.open('w', encoding='utf-8')
        self.stderr = self.err.open('w', encoding='utf-8')
        a, b = [backend.server_port for backend in backends]
        base = {'host': '127.0.0.1', 'port': a}
        routes = [dict(prefix='/a', strip_prefix=True, **base),
                  {'prefix': '/b', 'host': '127.0.0.1', 'port': b, 'strip_prefix': True},
                  {'prefix': '/round', 'strip_prefix': True,
                   'upstreams': [base, {'host': '127.0.0.1', 'port': b}]},
                  dict(prefix='/limited', rate_limit={'requests_per_second': 1, 'burst': 1}, **base),
                  {'prefix': '/stream', 'host': '127.0.0.1', 'port': b,
                   'stream_response': True, 'timeout_ms': 5000}]
        env = environment()
        env.update(APIGATE_LISTEN_ADDRESS='127.0.0.1', APIGATE_LISTEN_PORT='0',
                   APIGATE_METRICS_ENABLED='true', APIGATE_ROUTES=json.dumps(routes),
                   APIGATE_LOG_LEVEL='info', APIGATE_SHUTDOWN_GRACE_MS='1000')
        try:
            self.process = subprocess.Popen([str(binary)], env=env, stdout=subprocess.PIPE,
                                            stderr=self.stderr, text=True, bufsize=1,
                                            start_new_session=True)
            self.reader = threading.Thread(target=self.read_logs)
            self.reader.start()
        except BaseException:
            self.close()
            raise

    def read_logs(self):
        try:
            for line in self.process.stdout:
                self.output.write(line)
                self.output.flush()
                try:
                    event = json.loads(line)['payload']
                except (ValueError, KeyError, TypeError):
                    event = {'event': 'invalid_log'}
                with self.condition:
                    self.events.append(event)
                    self.condition.notify_all()
        finally:
            with self.condition:
                self.ended = True
                self.condition.notify_all()

    def wait(self, name, request_id=None):
        deadline = time.monotonic() + TIMEOUT
        with self.condition:
            while not STOP.is_set():
                for event in self.events:
                    if event.get('event') == name and (request_id is None or
                                                      event.get('request_id') == request_id):
                        return event
                remaining = deadline - time.monotonic()
                if self.ended or remaining <= 0:
                    break
                self.condition.wait(min(remaining, 0.2))
        if STOP.is_set():
            raise DemoStopped()
        raise DemoError(f'gateway event not observed: {name}; exit={self.process.poll()}')

    def close(self):
        if self.process is not None:
            stop_process(self.process)
        if self.reader is not None:
            self.reader.join(timeout=TIMEOUT)
            if self.reader.is_alive():
                raise DemoError('gateway log reader did not stop')
        if self.process is not None and self.process.stdout is not None:
            self.process.stdout.close()
        self.output.close()
        self.stderr.close()


def request(port, path, method='GET', body=None):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=TIMEOUT)
    try:
        connection.request(method, path, body=body, headers={'X-Request-ID': 'untrusted-client'})
        response = connection.getresponse()
        result = response.status, response.getheaders(), response.read()
        return result
    finally:
        connection.close()


def verify(gateway, port, backends):
    for path in ['/healthz', '/readyz']:
        status, _, _ = request(port, path)
        check(path, 200, status, status == 200)
    for path, marker in [('/a/echo?demo=1&raw=%2F', 'A'), ('/b/echo', 'B')]:
        status, headers, body = request(port, path)
        data = json.loads(body)
        check(f'route {path}', f'200 backend {marker}', f'{status} backend {data.get("backend")}',
              status == 200 and data.get('backend') == marker)
        if marker == 'A':
            check('strip_prefix + query', '/echo?demo=1&raw=%2F', data['target'],
                  data['target'] == '/echo?demo=1&raw=%2F')
        ids = [value for name, value in headers if name.lower() == 'x-request-id']
        valid = len(ids) == 1 and re.fullmatch(r'[0-9a-f]{32}-[0-9a-f]{16}', ids[0])
        check('response/backend ID', 'one identical gateway ID', ids,
              valid and data['request_ids'] == ids)
        event = gateway.wait('http_request_completed', ids[0])
        check('log ID', ids[0], event['request_id'], event['request_id'] == ids[0])
    markers = []
    for _ in range(4):
        status, _, body = request(port, '/round/echo')
        if status != 200:
            raise DemoError('round robin response failed')
        markers.append(json.loads(body)['backend'])
    check('round robin', ['A', 'B', 'A', 'B'], markers, markers == ['A', 'B', 'A', 'B'])
    status, _, body = request(port, '/a/body?original=yes', 'POST', b'demo-body')
    data = json.loads(body)
    check('POST body/path/query', '200 POST /body?original=yes demo-body', data,
          status == 200 and data['method'] == 'POST' and data['body'] == 'demo-body'
          and data['target'] == '/body?original=yes')
    status, headers, body = request(port, '/a/head', 'HEAD')
    length = dict((key.lower(), value) for key, value in headers).get('content-length', '0')
    check('HEAD', '200, metadata length > 0, no body', f'{status}, length={length}, body={body!r}',
          status == 200 and int(length) > 0 and body == b'')
    # One persistent connection, no intervening I/O or user input. The elapsed bound proves
    # fewer than one token could be replenished (1/s, burst=1); a slow host fails explicitly.
    client = http.client.HTTPConnection('127.0.0.1', port, timeout=TIMEOUT)
    try:
        started = time.monotonic()
        statuses = []
        for _ in range(2):
            client.request('GET', '/limited')
            response = client.getresponse()
            response.read()
            statuses.append(response.status)
        elapsed = time.monotonic() - started
        check('route rate limit (1/s, burst=1)', '[200, 429] in <1s',
              f'{statuses} in {elapsed:.3f}s', statuses == [200, 429] and elapsed < 1)
    finally:
        client.close()
    client = http.client.HTTPConnection('127.0.0.1', port, timeout=TIMEOUT)
    try:
        client.request('GET', '/stream')
        response = client.getresponse()
        upstream = backends[1]
        if not upstream.prefix_sent.wait(TIMEOUT):
            raise DemoError('backend did not publish the stream prefix')
        first = response.read(6)
        check('stream first body before tail release', b'first-', first,
              response.status == 200 and first == b'first-' and upstream.prefix_sent.is_set()
              and not upstream.release_tail.is_set())
        upstream.release_tail.set()
        tail = response.read()
        check('stream completion', b'last!', tail, tail == b'last!')
    finally:
        backends[1].release_tail.set()
        client.close()
    status, headers, body = request(port, '/metrics')
    check('metrics', '200 Prometheus apigate_*', f'{status}, {len(body)} bytes',
          status == 200 and b'apigate_http_responses_total' in body
          and 'version=0.0.4' in dict((k.lower(), v) for k, v in headers).get('content-type', ''))
    check('backend handlers', 'no errors', backends[0].errors + backends[1].errors,
          not any(backend.errors for backend in backends))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--binary', type=Path)
    args = parser.parse_args()
    for signum in [signal.SIGINT, signal.SIGTERM]:
        signal.signal(signum, lambda *_: STOP.set())
    binary = args.binary.resolve() if args.binary else build()
    if STOP.is_set():
        return 0
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise DemoError('--binary must name an executable file')
    with tempfile.TemporaryDirectory(prefix='apigate-demo-') as temporary:
        directory = Path(temporary)
        print(f'TEMP_DIR={directory}', flush=True)
        backends = []
        gateway = None
        verified = False
        try:
            for marker in ['A', 'B']:
                backend = Backend(marker)
                backends.append(backend)
                print(f'BACKEND_{marker}=http://127.0.0.1:{backend.server_port}', flush=True)
            gateway = Gateway(binary, directory, backends)
            print(f'GATEWAY_PID={gateway.process.pid}', flush=True)
            port = int(gateway.wait('http_listener_started')['port'])
            print(f'GATEWAY=http://127.0.0.1:{port}', flush=True)
            verify(gateway, port, backends)
            verified = True
            print('DEMO VERIFIED', flush=True)
            if not args.smoke:
                print(f"Services remain available. Logs: {gateway.log}\n"
                      f"curl -i http://127.0.0.1:{port}/a/hello?demo=1\n"
                      f"curl -s http://127.0.0.1:{port}/round/hello\n"
                      f"curl -s http://127.0.0.1:{port}/metrics\n"
                      f"Copy X-Request-ID, then: grep -F '<REQUEST_ID>' '{gateway.log}'\n"
                      'Ctrl+C to stop and remove the temporary directory.', flush=True)
                while not STOP.wait(0.2):
                    if gateway.process.poll() is not None:
                        raise DemoError('gateway exited during manual demo')
        except BaseException:
            if gateway is not None:
                print(f'Gateway diagnostics (last 8 lines):\n'
                      + '\n'.join(gateway.log.read_text().splitlines()[-8:])
                      + '\n' + gateway.err.read_text()[-2000:], file=sys.stderr)
            raise
        finally:
            # A second signal must not interrupt resource reclamation.
            for signum in [signal.SIGINT, signal.SIGTERM]:
                signal.signal(signum, signal.SIG_IGN)
            try:
                if gateway is not None:
                    gateway.close()
                    if verified and gateway.process.returncode != 0:
                        raise DemoError(f'gateway shutdown failed (exit {gateway.process.returncode})')
            finally:
                cleanup_errors = []
                for backend in backends:
                    try:
                        backend.close()
                    except Exception as error:
                        cleanup_errors.append(str(error))
                if cleanup_errors:
                    raise DemoError('; '.join(cleanup_errors))
    print('CLEANUP COMPLETE (gateway, backends, handles and temporary files)', flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except DemoStopped:
        print('Demo interrupted; resources reclaimed.', file=sys.stderr)
        sys.exit(0)
    except (DemoError, OSError, ValueError, http.client.HTTPException) as error:
        print(f'Demo failed: {error}', file=sys.stderr)
        sys.exit(1)
