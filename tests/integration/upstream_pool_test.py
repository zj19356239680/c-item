#!/usr/bin/env python3
"""Count real accepts/handshakes; no inferred reuse and no timing sleeps."""
from contextlib import ExitStack
import http.client
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

from http_proxy_test import ControlledUpstreamHandler, UpstreamState, stop_service
from http_server_test import ServiceProcess, build_test_environment, assert_normal_log_output
from https_proxy_test import certificate_files
from metrics_test import scrape


class PoolHandler(ControlledUpstreamHandler):
    def setup(self):
        if isinstance(self.request, ssl.SSLSocket):
            self.request.do_handshake()
            assert self.request.selected_alpn_protocol() == "http/1.1"
            with self.server.condition:
                self.server.handshakes += 1
                self.server.condition.notify_all()
        super().setup()

    def finish(self):
        try:
            super().finish()
        finally:
            with self.server.condition:
                self.server.closed += 1
                self.server.condition.notify_all()

    def _handle_request(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.state.record(self, body)
        route = self.path.split("?", 1)[0]
        if route == "/block":
            self.server.blocked.set()
            if not self.server.release.wait(timeout=8):
                return
        if route == "/truncated":
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nx")
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        if route == "/switch":
            self.connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
            self.close_connection = True
            return
        if route == "/extra":
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokunexpected")
            return
        if route == "/eof":
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nok")
            self.close_connection = True
            return
        self.send_response(200)
        if route == "/chunked":
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"2\r\nok\r\n0\r\n\r\n")
            return
        payload = self.server.identity.encode()
        self.send_header("Content-Length", str(len(payload)))
        if route == "/upgrade-header":
            self.send_header("Upgrade", "websocket")
        if route == "/close":
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)
        if route == "/idle-poison":
            # The client releases this only after observing the first final response and
            # a local metrics snapshot: the original exchange has already returned its lease.
            if not self.server.poison_release.wait(timeout=5):
                return
            self.connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\npoison")
            self.server.poison_sent.set()


class PoolBackend(http.server.ThreadingHTTPServer):
    daemon_threads = False

    def __init__(self, resources, root=None, identity="ok", certificate="server"):
        super().__init__(("127.0.0.1", 0), PoolHandler)
        self.state = UpstreamState()
        self.identity = identity
        self.condition = threading.Condition()
        self.accepts = self.handshakes = self.closed = 0
        self.peers = []
        self.errors = []
        self.blocked = threading.Event()
        self.release = threading.Event()
        self.poison_release = threading.Event()
        self.poison_sent = threading.Event()
        self.context = None
        if root is not None:
            self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.context.minimum_version = ssl.TLSVersion.TLSv1_2
            self.context.load_cert_chain(root / (certificate + ".pem"), root / (certificate + ".key"))
            self.context.set_alpn_protocols(["h2", "http/1.1"])
        self.worker = threading.Thread(target=self.serve_forever, daemon=True)
        self.worker.start()
        resources.callback(self.close)

    @property
    def port(self):
        return self.server_address[1]

    def get_request(self):
        peer, address = super().get_request()
        peer.settimeout(5)
        if self.context:
            try:
                peer = self.context.wrap_socket(peer, server_side=True, do_handshake_on_connect=False)
            except BaseException:
                peer.close()
                raise
        with self.condition:
            self.peers.append(peer)
            self.accepts += 1
            self.condition.notify_all()
        return peer, address

    def handle_error(self, request, client_address):
        error = sys.exception()
        if not isinstance(error, (ssl.SSLError, ConnectionError, OSError)):
            self.errors.append(type(error).__name__)

    def wait_closed(self, count):
        with self.condition:
            assert self.condition.wait_for(lambda: self.closed >= count, timeout=3), "upstream lease leaked"

    def close_peers(self):
        with self.condition:
            peers = list(self.peers)
        for peer in peers:
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def close(self):
        self.release.set()
        self.poison_release.set()
        self.close_peers()
        self.shutdown()
        self.server_close()
        self.worker.join(timeout=3)
        assert not self.worker.is_alive() and not self.errors, "backend cleanup failed"


def start(resources, binary, backend, enabled="true", routes=None, root=None, legacy=False, **extra):
    environment = {"APIGATE_METRICS_ENABLED": "true", **extra}
    if enabled is not None:
        environment["APIGATE_UPSTREAM_KEEP_ALIVE"] = enabled
    if legacy:
        environment.update(APIGATE_UPSTREAM_HOST="127.0.0.1", APIGATE_UPSTREAM_PORT=str(backend.port))
    else:
        environment["APIGATE_ROUTES"] = json.dumps(routes or [{"prefix": "/", "host": "127.0.0.1",
            "port": backend.port, "scheme": "https" if root else "http", "timeout_ms": 2000}])
    if root:
        environment["APIGATE_UPSTREAM_CA_FILE"] = str(root / "ca.pem")
    service = ServiceProcess(binary, environment)
    resources.callback(service.process.stdout.close)
    resources.callback(service.cleanup)
    port = int(service.wait_for_event("http_listener_started")["port"])

    def client():
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        resources.callback(connection.close)
        return connection
    return service, client


def fetch(client, target="/ok", method="GET", body=None, headers=None):
    connection = client()
    connection.request(method, target, body=body, headers=headers or {})
    response = connection.getresponse()
    status, payload = response.status, response.read()
    connection.close()
    return status, payload


def verify_logs(service):
    records = assert_normal_log_output("".join(service.lines))
    assert all(record["level"] not in ("error", "critical") for record in records)


def test_config(binary):
    for value in (None, "false", "true", "", "TRUE", "0", "false ", "private-invalid"):
        extra = {} if value is None else {"APIGATE_UPSTREAM_KEEP_ALIVE": value}
        result = subprocess.run([binary, "--check-config"], env=build_test_environment(extra),
                                capture_output=True, timeout=4, check=False)
        if value in (None, "false", "true"):
            assert result.returncode == 0
            assert json.loads(result.stdout)["payload"]["upstream_keep_alive"] == (value == "true")
        else:
            assert result.returncode != 0 and not result.stdout
            assert b"private-invalid" not in result.stderr
    result = subprocess.run([binary, "--help"], capture_output=True, timeout=4, check=False)
    assert result.returncode == 0 and b"APIGATE_UPSTREAM_KEEP_ALIVE" in result.stdout


def test_reuse(binary, root=None):
    for enabled in (None, "false", "true"):
        with ExitStack() as resources:
            backend = PoolBackend(resources, root)
            service, client = start(resources, binary, backend, enabled, root=root, legacy=root is None)
            for method in ("GET", "POST", "PUT", "PATCH"):
                status, body = fetch(client, "/ok?raw=%2f", method, None if method == "GET" else b"payload",
                                     {"Connection": "close, X-Private-Hop", "X-Private-Hop": "remove"})
                assert status == 200 and body == b"ok"
            expected = 1 if enabled == "true" else 4
            assert backend.accepts == expected
            assert backend.handshakes == (expected if root else 0)
            for _ in range(4):
                record = backend.state.requests.get(timeout=2)
                assert record["path"] == "/ok?raw=%2f"
                assert record["body"] in (b"", b"payload")
            stop_service(service)
            backend.wait_closed(expected)
            verify_logs(service)
            print(f"pool evidence: scheme={'https' if root else 'http'} enabled={enabled} requests=4 accepts={backend.accepts} handshakes={backend.handshakes}")


def test_isolation_and_framing(binary, root):
    with ExitStack() as resources:
        plain, tls = PoolBackend(resources, identity="plain"), PoolBackend(resources, root, "tls")
        routes = [{"prefix": "/mix", "strip_prefix": True, "upstreams": [
            {"host": "127.0.0.1", "port": plain.port},
            {"host": "localhost", "port": tls.port, "scheme": "https"}]},
            {"prefix": "/same", "strip_prefix": True, "host": "127.0.0.1", "port": plain.port}]
        service, client = start(resources, binary, plain, routes=routes, root=root)
        for _ in range(2):
            assert fetch(client, "/mix/ok") == (200, b"plain")
            assert fetch(client, "/mix/ok") == (200, b"tls")
        assert fetch(client, "/same/ok") == (200, b"plain")
        assert plain.accepts == tls.accepts == tls.handshakes == 1
        stop_service(service)
        plain.wait_closed(1)
        tls.wait_closed(1)
        verify_logs(service)
    for root_or_none in (None, root):
        with ExitStack() as resources:
            backend = PoolBackend(resources, root_or_none)
            service, client = start(resources, binary, backend, root=root_or_none)
            assert fetch(client, "/chunked") == (200, b"ok")
            assert fetch(client)[0] == 200 and backend.accepts == 1
            for path, expected in (("/close", 200), ("/truncated", 502), ("/extra", 200),
                                   ("/switch", 502), ("/upgrade-header", 200)):
                assert fetch(client, path)[0] == expected
                before = backend.accepts
                assert fetch(client)[0] == 200
                assert backend.accepts == before + 1, path + " was incorrectly cached"
            if root_or_none is None:
                assert fetch(client, "/eof") == (200, b"ok")
                before = backend.accepts
                assert fetch(client)[0] == 200 and backend.accepts == before + 1
            # Close a known idle connection AFTER the last response was returned.
            before = backend.accepts
            backend.close_peers()
            backend.wait_closed(before)
            assert fetch(client, "/must-not-retry", "POST", b"one-attempt")[0] == 502
            assert backend.accepts == before  # stale failure never opens a retry connection
            assert fetch(client) == (200, b"ok")
            assert backend.accepts == before + 1
            stop_service(service)
            backend.wait_closed(backend.accepts)
            verify_logs(service)


def wait_for_queued_response(service, backend):
    # Linux-only: observe the gateway socket's actual RX queue without reading its bytes.
    # sendall alone does not establish that TCP/TLS data is ready on the receiving socket.
    path = Path(f"/proc/{service.process.pid}/net/tcp")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        assert service.process.poll() is None
        for row in path.read_text().splitlines()[1:]:
            fields = row.split()
            remote_address, remote_port = fields[2].split(":")
            queued_bytes = int(fields[4].split(":")[1], 16)
            if remote_address == "0100007F" and int(remote_port, 16) == backend.port and queued_bytes > 0:
                return
    raise AssertionError("extra upstream response was not ready in the gateway socket RX queue")


def test_idle_response_pollution(binary, root=None):
    with ExitStack() as resources:
        backend = PoolBackend(resources, root)
        service, client = start(resources, binary, backend, root=root)
        assert fetch(client, "/idle-poison") == (200, b"ok")
        first = backend.state.requests.get(timeout=2)
        assert first["path"] == "/idle-poison" and backend.accepts == 1
        snapshot = scrape(client())
        assert snapshot["apigate_active_proxies"] == 0
        assert snapshot["apigate_in_flight_body_bytes"] == 0
        backend.poison_release.set()
        assert backend.poison_sent.wait(timeout=3)
        wait_for_queued_response(service, backend)
        # Round trip locally after readiness, without consuming any upstream bytes.
        scrape(client())
        actual = fetch(client, "/actual-second", "POST", b"sent-once")
        print(f"idle pollution evidence: scheme={'https' if root else 'http'} second={actual!r} accepts={backend.accepts}", flush=True)
        assert actual == (200, b"ok"), f"idle response contaminated new request: {actual!r}"
        assert backend.accepts == 2
        backend.wait_closed(1)
        second = backend.state.requests.get(timeout=2)
        assert second["path"] == "/actual-second" and second["body"] == b"sent-once"
        assert fetch(client) == (200, b"ok") and backend.accepts == 2
        third = backend.state.requests.get(timeout=2)
        assert third["path"] == "/ok" and backend.state.requests.empty()
        assert backend.handshakes == (2 if root else 0)
        stop_service(service)
        backend.wait_closed(2)
        verify_logs(service)


def test_tls_rejection(binary, root):
    for certificate, ca in (("mismatch", "ca.pem"), ("server", "other-ca.pem")):
        with ExitStack() as resources:
            backend = PoolBackend(resources, root, certificate=certificate)
            routes = [{"prefix": "/", "host": "localhost", "port": backend.port, "scheme": "https"}]
            service, client = start(resources, binary, backend, routes=routes,
                                    APIGATE_UPSTREAM_CA_FILE=str(root / ca))
            assert fetch(client)[0] == 502
            assert fetch(client)[0] == 502
            assert backend.accepts == 2  # rejected TLS transports never enter the pool
            stop_service(service)
            verify_logs(service)


def test_exclusive_and_shutdown(binary, root):
    for mode in ("normal", "deadline", "second_signal"):
        with ExitStack() as resources:
            backend = PoolBackend(resources, root)
            service, client = start(resources, binary, backend, root=root,
                APIGATE_MAX_CONCURRENT_PROXIES="2", APIGATE_MAX_IN_FLIGHT_BYTES="128",
                APIGATE_SHUTDOWN_GRACE_MS="150" if mode == "deadline" else "3000")
            # One active lease cannot be borrowed by the next downstream client.
            held = client()
            held.request("POST", "/block", body=b"x" * 64)
            assert backend.blocked.wait(timeout=3)
            assert fetch(client)[0] == 200 and backend.accepts == 2
            values = scrape(client())
            assert values["apigate_active_proxies"] == 1 and values["apigate_in_flight_body_bytes"] == 64
            backend.release.set()
            response = held.getresponse()
            assert response.status == 200 and response.read() == b"ok"
            values = scrape(client())
            assert values["apigate_active_proxies"] == values["apigate_in_flight_body_bytes"] == 0
            assert fetch(client)[0] == 200 and backend.accepts == 2
            backend.release.clear()
            backend.blocked.clear()
            held.request("GET", "/block")
            assert backend.blocked.wait(timeout=3)
            service.terminate()
            service.wait_for_event("shutdown_drain_started")
            backend.wait_closed(1)  # idle lease closed while active work is still pending
            if mode == "normal":
                backend.release.set()
                response = held.getresponse()
                assert response.status == 200 and response.read() == b"ok"
            elif mode == "second_signal":
                service.terminate(signal.SIGINT)
                service.wait_for_event("shutdown_force_requested")
            assert service.process.wait(timeout=4) == 0
            backend.release.set()
            backend.wait_closed(2)
            service.finish_log_reader()
            verify_logs(service)
            print(f"pool shutdown evidence: {mode} accepts=2 closed={backend.closed}")


def main():
    binary = os.path.abspath(sys.argv[1])
    test_config(binary)
    test_reuse(binary)
    test_idle_response_pollution(binary)
    with tempfile.TemporaryDirectory(prefix="apigate-pool-tls-") as directory:
        root = certificate_files(directory)
        test_reuse(binary, root)
        test_idle_response_pollution(binary, root)
        test_isolation_and_framing(binary, root)
        test_tls_rejection(binary, root)
        test_exclusive_and_shutdown(binary, root)
    print("upstream pool: accepts, TLS handshakes, no retry, framing, leases and shutdown passed")


if __name__ == "__main__":
    main()
