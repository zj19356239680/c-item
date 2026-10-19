#!/usr/bin/env python3

"""Controlled active-health tests; no public network or timing sleeps."""

from contextlib import ExitStack
import http.client
import json
import os
import secrets
import select
import ssl
import subprocess
import sys
import tempfile
import threading
import time

from http_proxy_test import ControlledUpstream, ControlledUpstreamHandler, stop_service
from http_server_test import (
    ServiceProcess, assert_event_absent, assert_normal_log_output,
    assert_sensitive_values_absent, build_test_environment, read_json_response,
)
from https_proxy_test import TlsUpstream, TlsHandler, StalledHandshake, certificate_files


class HealthControl:
    def __init__(self):
        self.condition = threading.Condition()
        self.mode = "healthy"
        self.records = []
        self.gates = []
        self.closed = 0

    def set_mode(self, mode):
        with self.condition:
            self.mode = mode

    def wait_count(self, count, path=None, timeout=3):
        with self.condition:
            return self.condition.wait_for(
                lambda: sum(path is None or item["path"] == path for item in self.records) >= count,
                timeout=timeout)

    def release_all(self):
        with self.condition:
            for gate in self.gates:
                gate.set()

    def handle(self, handler):
        with self.condition:
            mode = self.mode
            gate = threading.Event()
            self.gates.append(gate)
            self.records.append({"path": handler.path, "method": handler.command,
                                 "host": handler.headers.get("Host"),
                                 "authorization": handler.headers.get("Authorization"),
                                 "cookie": handler.headers.get("Cookie"),
                                 "content_length": handler.headers.get("Content-Length")})
            self.condition.notify_all()
        handler.close_connection = True
        try:
            if mode == "gated" and not gate.wait(timeout=5):
                return
            if mode not in ("blocked", "gated") or gate.is_set():
                if mode == "informational":
                    data = b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"
                elif mode == "malformed":
                    data = b"not-http\r\n\r\n"
                elif mode == "oversized":
                    data = b"HTTP/1.1 200 OK\r\nX-Large: " + b"a" * 17000 + b"\r\n\r\n"
                else:
                    status = 503 if mode == "failed" else (302 if mode == "redirect" else 200)
                    # Deliberately advertise a huge body and never send it: header-only probes
                    # must succeed without consuming the business body reservation budget.
                    data = (f"HTTP/1.1 {status} Test\r\nContent-Length: 1048576\r\n"
                            "Location: /must-not-follow\r\nConnection: close\r\n\r\n").encode()
                handler.connection.sendall(data)
            handler.connection.settimeout(3)
            try:
                assert handler.connection.recv(1) == b"", "probe sent unexpected body"
            except (ConnectionError, ssl.SSLError):
                pass
            with self.condition:
                self.closed += 1
                self.condition.notify_all()
        except (ConnectionError, ssl.SSLError):
            pass


class HealthHandler(ControlledUpstreamHandler):
    def _handle_request(self):
        if self.path.startswith("/probe-"):
            self.server.health.handle(self)
        else:
            super()._handle_request()


class TlsHealthHandler(HealthHandler, TlsHandler):
    pass


def backend(resources, tls_root=None, certificate="server"):
    upstream = ControlledUpstream() if tls_root is None else TlsUpstream(tls_root, certificate)
    upstream.server.RequestHandlerClass = HealthHandler if tls_root is None else TlsHealthHandler
    # Reuse the existing server, but track and explicitly join every request worker with
    # one cleanup deadline rather than using server_close's unbounded worker joins.
    upstream.server.daemon_threads = True
    upstream.server.health = HealthControl()
    workers = []
    worker_lock = threading.Lock()
    original_worker = upstream.server.process_request_thread

    def dispatch_worker(request, address):
        worker = threading.Thread(target=original_worker, args=(request, address), daemon=True)
        with worker_lock:
            workers.append(worker)
        worker.start()

    upstream.server.process_request = dispatch_worker

    def close():
        upstream.server.health.release_all()
        upstream.close()
        deadline = time.monotonic() + 4
        with worker_lock:
            remaining_workers = list(workers)
        for worker in remaining_workers:
            worker.join(timeout=max(0, deadline - time.monotonic()))
        assert all(not worker.is_alive() for worker in remaining_workers), "health backend worker leaked"
        if tls_root is not None:
            assert upstream.server.unexpected.empty(), "unexpected TLS backend worker error"

    upstream.start()
    resources.callback(close)
    return upstream


def service_client(resources, binary, routes, **extra):
    service = ServiceProcess(binary, {"APIGATE_ROUTES": json.dumps(routes), **extra})
    resources.callback(service.process.stdout.close)
    resources.callback(service.cleanup)
    port = int(service.wait_for_event("http_listener_started")["port"])

    def client():
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        resources.callback(connection.close)
        return connection
    return service, client


def health(path, interval=100, timeout=100, unhealthy=2, healthy=2):
    return {"path": path, "interval_ms": interval, "timeout_ms": timeout,
            "unhealthy_threshold": unhealthy, "healthy_threshold": healthy}


def transition(service, event, route=0, upstream=0, timeout=4):
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        assert remaining > 0, "health transition was not observed"
        record = service.wait_for_event(event, timeout=remaining)
        if (record["route_index"], record["upstream_index"]) == (route, upstream):
            return record


def check_logs(service, markers=None):
    output = "".join(service.lines)
    records = assert_normal_log_output(output)
    assert all(record["level"] not in ("error", "critical") for record in records)
    for event in ("http_upstream_request_failed", "http_response_write_failed",
                  "http_accept_retry_failed", "http_proxy_rejected_capacity", "http_buffer_budget_rejected"):
        assert_event_absent(output, event)
    assert_sensitive_values_absent(output, markers or {})
    return [record["payload"] for record in records]


def check_probes(upstream, host="127.0.0.1"):
    for record in upstream.server.health.records:
        assert record["method"] == "GET" and record["path"].startswith("/probe-")
        assert record["host"] == f"{host}:{upstream.port}"
        assert record["authorization"] is None and record["cookie"] is None
        assert record["content_length"] in (None, "0")


def test_ejection_and_recovery(binary):
    with ExitStack() as resources:
        first, second = backend(resources), backend(resources)
        first.state.identity, second.state.identity = "health-backend-A", "health-backend-B"
        marker = secrets.token_hex(12)
        probe_path = "/probe-" + marker
        limited_probe = "/probe-limited-" + marker
        pool = [{"host": "127.0.0.1", "port": upstream.port} for upstream in (first, second)]
        routes = [{"prefix": "/api", "strip_prefix": True, "upstreams": pool,
                   "health_check": health(probe_path)},
                  {"prefix": "/limited", "strip_prefix": True, **pool[0],
                   "health_check": health(limited_probe),
                   "rate_limit": {"requests_per_second": 1, "burst": 1}},
                  {"prefix": "/unchecked", "strip_prefix": True, **pool[1]}]
        service, client = service_client(resources, binary, routes, APIGATE_MAX_CONCURRENT_PROXIES="2",
                                          APIGATE_MAX_IN_FLIGHT_BYTES="128")
        connection, holder = client(), client()
        assert first.server.health.wait_count(3, limited_probe)
        response, _ = read_json_response(connection, "GET", "/limited/first")
        assert response.status == 201  # multiple probes did not consume the burst of one
        response, payload = read_json_response(connection, "GET", "/limited/second")
        assert response.status == 429 and payload["error"]["code"] == "rate_limited"

        holder.request("GET", "/api/blocked")
        assert first.state.block_started.wait(timeout=2)
        first.server.health.set_mode("failed")
        transition(service, "http_upstream_marked_unhealthy", upstream=0)
        assert not select.select([holder.sock], [], [], 0)[0], "ejection cancelled an admitted request"
        for _ in range(4):
            response, payload = read_json_response(connection, "POST", "/api/item?x=" + marker,
                                                   marker.encode(), {"Authorization": "test-" + marker,
                                                                     "Cookie": "test=" + marker})
            assert response.status == 201 and payload == {"upstream": second.state.identity}
        second.server.health.set_mode("failed")
        transition(service, "http_upstream_marked_unhealthy", upstream=1)
        before = (first.state.request_count(), second.state.request_count())
        for _ in range(4):
            response, payload = read_json_response(connection, "POST", "/api/missing", b"x" * 128)
            assert response.status == 503 and payload == {"error": {
                "code": "no_healthy_upstream", "message": "no healthy upstream is available"}}
            assert response.getheader("Retry-After") is None
        assert before == (first.state.request_count(), second.state.request_count())
        # All-down releases the proxy slot immediately and full body budget after writing.
        connection.request("POST", "/unchecked/no-content", body=b"x" * 128)
        response = connection.getresponse()
        assert response.status == 204 and response.read() == b""
        before = (first.state.request_count(), second.state.request_count())
        for path in ("/healthz", "/readyz"):
            assert read_json_response(connection, "GET", path)[0].status == 200
        assert before == (first.state.request_count(), second.state.request_count())

        first.server.health.set_mode("healthy")
        transition(service, "http_upstream_marked_healthy", upstream=0)
        for _ in range(2):
            response, payload = read_json_response(connection, "GET", "/api/restored")
            assert response.status == 201 and payload == {"upstream": first.state.identity}
        first.state.release_block.set()
        held = holder.getresponse()
        assert held.status == 200 and json.loads(held.read()) == {"upstream": "released"}
        second.server.health.set_mode("healthy")
        transition(service, "http_upstream_marked_healthy", upstream=1)
        identities = [read_json_response(connection, "GET", "/api/rejoined")[1]["upstream"]
                      for _ in range(2)]
        assert identities == [second.state.identity, first.state.identity]
        connection.close()
        holder.close()
        stop_service(service)
        records = check_logs(service, {"path/query/body": marker, "first response": first.state.identity,
                                       "second response": second.state.identity})
        transitions = [record for record in records if record["event"].startswith("http_upstream_marked_")
                       and record["route_index"] == 0]
        assert [(record["event"], record["upstream_index"]) for record in transitions] == [
            ("http_upstream_marked_unhealthy", 0), ("http_upstream_marked_unhealthy", 1),
            ("http_upstream_marked_healthy", 0), ("http_upstream_marked_healthy", 1)]
        check_probes(first)
        check_probes(second)


def test_probe_protocol_and_timeout(binary):
    for mode, reason in (("informational", "status"), ("redirect", "status"),
                         ("malformed", "read"), ("oversized", "read"), ("blocked", "timeout")):
        with ExitStack() as resources:
            upstream = backend(resources)
            upstream.server.health.set_mode(mode)
            routes = [{"prefix": "/", "host": "127.0.0.1", "port": upstream.port,
                       "health_check": health("/probe-protocol", unhealthy=1)}]
            service, client = service_client(resources, binary, routes)
            assert transition(service, "http_upstream_marked_unhealthy")["reason"] == reason
            response, payload = read_json_response(client(), "GET", "/business")
            assert response.status == 503 and payload["error"]["code"] == "no_healthy_upstream"
            assert upstream.state.request_count() == 0
            stop_service(service)
            records = check_logs(service)
            assert sum(record["event"] == "http_upstream_marked_unhealthy" for record in records) == 1


def test_global_schedule_and_stop(binary):
    with ExitStack() as resources:
        upstream = backend(resources)
        control = upstream.server.health
        control.set_mode("gated")
        pool = [{"host": "127.0.0.1", "port": upstream.port}] * 4
        routes = [{"prefix": "/a", "upstreams": pool,
                   "health_check": health("/probe-a", 60000, 60000)},
                  {"prefix": "/b", "upstreams": pool,
                   "health_check": health("/probe-b", 60000, 60000)},
                  {"prefix": "/unchecked", "strip_prefix": True, **pool[0]}]
        service, client = service_client(resources, binary, routes, APIGATE_MAX_CONCURRENT_PROXIES="1",
                                          APIGATE_MAX_IN_FLIGHT_BYTES="128")
        assert control.wait_count(4)
        # Bounded negative observation: four confirmed, blocked probes must prevent a fifth.
        assert not control.wait_count(5, timeout=0.15)
        connection = client()
        connection.request("POST", "/unchecked/no-content", body=b"x" * 128)
        response = connection.getresponse()
        assert response.status == 204 and response.read() == b""
        for index in range(4):
            with control.condition:
                control.gates[index].set()
            assert control.wait_count(index + 5)
        assert sum(item["path"] == "/probe-b" for item in control.records) == 4
        connection.close()
        service.terminate()
        assert service.process.wait(timeout=3) == 0  # outstanding probes have 60-second deadlines
        service.finish_log_reader()
        records = check_logs(service)
        assert not any(record["event"].startswith("http_upstream_marked_") for record in records)
        control.release_all()
        check_probes(upstream)


def test_https_probes(binary, root):
    with ExitStack() as resources:
        good, bad = backend(resources, root), backend(resources, root, "mismatch")
        pool = lambda upstream: {"host": "localhost", "port": upstream.port, "scheme": "https"}
        routes = [{"prefix": "/secure", "strip_prefix": True, **pool(good),
                   "health_check": health("/probe-secure", unhealthy=1)},
                  {"prefix": "/bad", **pool(bad), "health_check": health("/probe-bad", unhealthy=1)}]
        service, client = service_client(resources, binary, routes, APIGATE_UPSTREAM_CA_FILE=str(root / "ca.pem"))
        assert good.server.health.wait_count(2)
        assert transition(service, "http_upstream_marked_unhealthy", route=1)["reason"] == "handshake"
        connection = client()
        response, _ = read_json_response(connection, "POST", "/secure/item?x=%2f&x=2", b"payload")
        assert response.status == 201
        received = good.state.requests.get(timeout=2)
        assert received["path"] == "/item?x=%2f&x=2" and received["body"] == b"payload"
        assert read_json_response(connection, "GET", "/bad/item")[0].status == 503
        assert bad.state.request_count() == 0
        connection.close()
        stop_service(service)
        check_logs(service, {"CA path": str(root), "health path": "/probe-secure"})
        check_probes(good, "localhost")
        assert good.server.sni.get(timeout=2) == "localhost"

        for ca in (None, root / "other-ca.pem"):
            with ExitStack() as processes:
                extra = {} if ca is None else {"APIGATE_UPSTREAM_CA_FILE": str(ca)}
                service, client = service_client(processes, binary, [routes[0]], **extra)
                assert transition(service, "http_upstream_marked_unhealthy")["reason"] == "handshake"
                assert read_json_response(client(), "GET", "/secure/rejected")[0].status == 503
                stop_service(service)
                check_logs(service)

    with ExitStack() as resources:
        stalled = StalledHandshake()
        resources.callback(stalled.close)
        routes = [{"prefix": "/", "host": "127.0.0.1", "port": stalled.port, "scheme": "https",
                   "health_check": health("/probe-handshake", 500, 150, unhealthy=1)}]
        service, client = service_client(resources, binary, routes, APIGATE_UPSTREAM_CA_FILE=str(root / "ca.pem"))
        assert stalled.started.wait(timeout=3)
        assert transition(service, "http_upstream_marked_unhealthy")["reason"] == "timeout"
        assert stalled.closed.wait(timeout=2)
        assert read_json_response(client(), "GET", "/rejected")[0].status == 503
        stop_service(service)
        check_logs(service)


def test_config_is_local_only(binary):
    path = "/probe-private-" + secrets.token_hex(12)
    route = {"prefix": "/", "host": "does-not-resolve.invalid", "port": 1, "health_check": health(path)}
    result = subprocess.run([binary, "--check-config"], capture_output=True, text=True, timeout=3,
                            env=build_test_environment({"APIGATE_ROUTES": json.dumps([route])}), check=False)
    assert result.returncode == 0 and not result.stderr
    assert json.loads(result.stdout)["payload"]["route_count"] == 1
    assert path not in result.stdout and route["host"] not in result.stdout
    route["health_check"]["path"] = path + "?invalid"
    for arguments in ([], ["--check-config"]):
        failed = subprocess.run([binary, *arguments], capture_output=True, text=True, timeout=3,
                                env=build_test_environment({"APIGATE_ROUTES": json.dumps([route])}), check=False)
        assert failed.returncode != 0 and not failed.stdout
        assert json.loads(failed.stderr)["category"] == "configuration"
        assert path not in failed.stderr and route["host"] not in failed.stderr


def test_one_deadline_across_tls_and_headers(binary, root):
    with ExitStack() as resources:
        upstream = backend(resources, root)
        upstream.server.health.set_mode("blocked")
        hello = threading.Event()
        release = threading.Event()
        original_callback = upstream.server.context.sni_callback

        def delayed_handshake(peer, name, context):
            original_callback(peer, name, context)
            hello.set()
            assert release.wait(timeout=3), "test handshake gate was not released"

        upstream.server.context.set_servername_callback(delayed_handshake)
        routes = [{"prefix": "/", "host": "localhost", "port": upstream.port, "scheme": "https",
                   "health_check": health("/probe-total-deadline", 2500, 1500, unhealthy=1)}]
        service, _ = service_client(resources, binary, routes,
                                    APIGATE_UPSTREAM_CA_FILE=str(root / "ca.pem"))
        resources.callback(release.set)
        assert hello.wait(timeout=3)  # actual ClientHello, not an assumed accepted connection
        # Deliberate transport latency injection, NOT a sleep used to infer probe state.
        # Consume 900ms inside TLS; the header stage must inherit what remains of 1500ms.
        assert not release.wait(timeout=0.9)
        release.set()
        result = transition(service, "http_upstream_marked_unhealthy", timeout=1.0)
        assert result["reason"] == "timeout"
        stop_service(service)
        check_logs(service)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: health_check_test.py <api-gate-binary>")
    binary = os.path.abspath(sys.argv[1])
    test_config_is_local_only(binary)
    test_ejection_and_recovery(binary)
    test_probe_protocol_and_timeout(binary)
    test_global_schedule_and_stop(binary)
    with tempfile.TemporaryDirectory(prefix="apigate-health-tls-") as directory:
        root = certificate_files(directory)
        test_https_probes(binary, root)
        test_one_deadline_across_tls_and_headers(binary, root)
    print("active health: thresholds, ejection/rejoin, header-only probes, fair cap, TLS and bounded stop passed")


if __name__ == "__main__":
    main()
