#!/usr/bin/env python3

import http.client
import json
import os
import secrets
import select
import subprocess
import sys
import threading
import time
from contextlib import ExitStack

from http_proxy_test import ControlledUpstream, stop_service
from http_server_test import (
    ServiceProcess, assert_event_absent, assert_normal_log_output,
    assert_sensitive_values_absent, build_test_environment, read_json_response,
)


def verify_configuration(binary):
    marker = "pool-config-" + secrets.token_hex(12)
    route = {"prefix": "/" + marker,
             "upstreams": [{"host": marker + ".test", "port": 9001},
                           {"host": "127.0.0.1", "port": 9002, "timeout_ms": 2000}]}
    raw = json.dumps([route])
    checked = subprocess.run([binary, "--check-config"], capture_output=True, text=True,
                             timeout=5, check=False,
                             env=build_test_environment({"APIGATE_ROUTES": raw}))
    assert checked.returncode == 0 and not checked.stderr
    payload = json.loads(checked.stdout)["payload"]
    assert payload["proxy_enabled"] is True and payload["route_count"] == 1
    assert "upstream_timeout_ms" not in payload and marker not in checked.stdout
    route["timeout_ms"] = None  # Presence conflicts even when the value is null.
    invalid = subprocess.run([binary], capture_output=True, text=True, timeout=5, check=False,
                             env=build_test_environment({"APIGATE_ROUTES": json.dumps([route])}))
    assert invalid.returncode != 0 and not invalid.stdout
    assert json.loads(invalid.stderr)["category"] == "configuration"
    assert marker not in invalid.stderr and "conflicts" in invalid.stderr


def test_round_robin(binary):
    with ExitStack() as resources:
        backends = []
        for identity in ("instance-a", "instance-b"):
            backend = ControlledUpstream()
            backend.start()
            resources.callback(backend.close)
            backend.state.identity = identity
            backends.append(backend)
        first, second = backends
        pool = [{"host": "127.0.0.1", "port": backend.port} for backend in backends]
        routes = [{"prefix": path, "strip_prefix": True, "upstreams": pool}
                  for path in ("/api", "/capacity", "/failure")]
        routes += [
            {"prefix": "/limited", "strip_prefix": True, "upstreams": pool,
             "rate_limit": {"requests_per_second": 1, "burst": 2}},
            {"prefix": "/timeouts", "strip_prefix": True,
             "upstreams": [{**pool[0], "timeout_ms": 100}, {**pool[1], "timeout_ms": 1500}]},
            {"prefix": "/healthz", "upstreams": pool},
            {"prefix": "/readyz", "upstreams": pool},
        ]
        service = ServiceProcess(binary, {"APIGATE_ROUTES": json.dumps(routes),
                                           "APIGATE_MAX_CONCURRENT_PROXIES": "1"})
        resources.callback(service.process.stdout.close)
        resources.callback(service.cleanup)
        port = int(service.wait_for_event("http_listener_started")["port"])

        def client():
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            resources.callback(connection.close)
            return connection

        connection, other, holder = client(), client(), client()
        marker = "pool-query-body-" + secrets.token_hex(12)

        def counts():
            return tuple(backend.state.request_count() for backend in backends)

        def forward(connection, path, expected, backend, method="GET", body=None):
            before = counts()
            response, payload = read_json_response(connection, method, path, body=body,
                                                   headers={"Host": "untrusted.invalid:1234"})
            assert response.status == 201 and payload == {"upstream": backend.state.identity}
            received = backend.state.requests.get(timeout=3)
            assert received["path"] == expected and received["method"] == method
            assert received["host"] == f"127.0.0.1:{backend.port}"
            assert received["body"] == (body or b"")
            if method != "GET":
                assert received["content_length"] == str(len(body or b""))
            after = counts()
            assert sum(after) == sum(before) + 1
            assert after[backends.index(backend)] == before[backends.index(backend)] + 1

        def rejected(connection, path, status, message):
            before = counts()
            response, payload = read_json_response(connection, "GET", path)
            assert response.status == status and payload["error"]["message"] == message
            assert payload["error"]["code"] == ("rate_limited" if status == 429 else "gateway_overloaded")
            assert counts() == before
            if status == 429:
                assert response.getheader("Retry-After") == "1"

        # Same keep-alive connection: A, B; a different client then shares the next A position.
        forward(connection, "/api/42?x=%2F&x=2&empty=&marker=" + marker,
                "/42?x=%2F&x=2&empty=&marker=" + marker, first)
        original_socket = connection.sock
        forward(connection, "/api/43?", "/43?", second, "POST", marker.encode("ascii"))
        assert connection.sock is original_socket
        forward(other, "/api/44", "/44", first)
        before = counts()
        for path in ("/healthz", "/readyz"):
            response, _ = read_json_response(connection, "GET", path)
            assert response.status == 200
        response, _ = read_json_response(connection, "GET", "/missing")
        assert response.status == 404 and counts() == before
        forward(connection, "/api/healthz", "/healthz", second)

        # One route-level bucket covers both instances and all clients.
        forward(connection, "/limited/first", "/first", first)
        forward(other, "/limited/second", "/second", second)
        rejected(connection, "/limited/third?marker=" + marker, 429, "request rate limit is exceeded")
        rejected(other, "/limited/fourth", 429, "request rate limit is exceeded")
        rejected(connection, "/limited/fifth", 429, "request rate limit is exceeded")

        # Global capacity rejection must not move /capacity from B back to A.
        holder.request("GET", "/capacity/blocked")
        assert first.state.block_started.wait(timeout=2)
        assert first.state.requests.get(timeout=2)["path"] == "/blocked"
        rejected(connection, "/capacity/rejected", 503, "proxy capacity is exhausted")
        first.state.release_block.set()
        held_response = holder.getresponse()
        assert held_response.status == 200
        assert json.loads(held_response.read()) == {"upstream": "released"}
        forward(connection, "/capacity/next", "/next", second)
        first.state.block_started.clear()
        first.state.release_block.clear()

        # A protocol failure is returned directly, without automatically visiting B.
        before = counts()
        response, payload = read_json_response(connection, "GET", "/failure/informational-100")
        assert response.status == 502 and payload["error"]["code"] == "bad_gateway"
        assert counts() == (before[0] + 1, before[1])
        assert first.state.requests.get(timeout=2)["path"] == "/informational-100"
        forward(connection, "/failure/next", "/next", second)
        forward(connection, "/failure/not-ejected", "/not-ejected", first)

        # A's 100ms deadline must finish before the 500ms client bound.
        connection.sock.settimeout(0.5)
        before = counts()
        response, payload = read_json_response(connection, "GET", "/timeouts/blocked")
        assert response.status == 504 and payload["error"]["code"] == "gateway_timeout"
        assert first.state.block_started.wait(timeout=1)
        assert first.state.requests.get(timeout=1)["path"] == "/blocked"
        assert counts() == (before[0] + 1, before[1])
        connection.sock.settimeout(3)

        # B's 1500ms deadline must not inherit A's 100ms. Observe a blocked real response
        # for 300ms, then release it. This bounded absence check tests deadline behavior,
        # not an arbitrary sleep used to infer acceptance or completion.
        connection.request("GET", "/timeouts/blocked")
        assert second.state.block_started.wait(timeout=2)
        assert second.state.requests.get(timeout=2)["path"] == "/blocked"
        assert not select.select([connection.sock], [], [], 0.3)[0]
        second.state.release_block.set()
        response = connection.getresponse()
        assert response.status == 200 and json.loads(response.read()) == {"upstream": "released"}
        first.state.release_block.set()

        # If quota has not refilled yet, pace bounded retries by its actual advice.
        deadline = time.monotonic() + 5
        retry_wait = threading.Event()
        while True:
            before = counts()
            response, payload = read_json_response(connection, "GET", "/limited/recovered")
            if response.status == 201:
                assert payload == {"upstream": first.state.identity}
                assert first.state.requests.get(timeout=2)["path"] == "/recovered"
                assert counts() == (before[0] + 1, before[1])
                break
            assert response.status == 429 and payload["error"]["code"] == "rate_limited"
            assert counts() == before
            remaining = deadline - time.monotonic()
            assert remaining > 0, "limited route did not recover"
            retry_after = int(response.getheader("Retry-After"))
            assert retry_after >= 1
            retry_wait.wait(timeout=min(retry_after, remaining))
        assert connection.sock is original_socket
        connection.close()
        other.close()
        holder.close()
        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        assert all(record["level"] not in ("error", "critical") for record in records)
        failures = [record["payload"] for record in records
                    if record["payload"].get("event") == "http_upstream_request_failed"]
        assert sorted(failure["stage"] for failure in failures) == ["invalid_response", "timeout"]
        for event in ("http_response_write_failed", "http_accept_retry_failed", "http_buffer_budget_rejected"):
            assert_event_absent(output, event)
        assert_sensitive_values_absent(output, {"query_body": marker, "route": "/api",
                                                "first": first.state.identity, "second": second.state.identity})
        assert service.process.poll() == 0


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: load_balance_test.py <api-gate-binary>")
    binary = os.path.abspath(sys.argv[1])
    verify_configuration(binary)
    test_round_robin(binary)
    print("round robin: shared sequence, 429/503 positions, Host/body/query, no retry and per-backend deadlines passed")


if __name__ == "__main__":
    main()
