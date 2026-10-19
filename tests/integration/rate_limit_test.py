#!/usr/bin/env python3

import http.client
import json
import os
import secrets
import subprocess
import sys
import threading
import time
from contextlib import ExitStack

from http_proxy_test import ControlledUpstream, stop_service
from http_server_test import (
    ServiceProcess,
    assert_event_absent,
    assert_normal_log_output,
    assert_sensitive_values_absent,
    build_test_environment,
    read_json_response,
)


def verify_configuration(binary):
    marker = "rate-config-" + secrets.token_hex(12)
    entry = {"prefix": "/" + marker, "host": marker + ".test", "port": 80,
             "rate_limit": {"requests_per_second": 10, "burst": 20}}
    raw = json.dumps([entry])
    valid = subprocess.run([binary, "--check-config"],
                           env=build_test_environment({"APIGATE_ROUTES": raw}),
                           capture_output=True, text=True, timeout=5, check=False)
    assert valid.returncode == 0 and not valid.stderr
    assert json.loads(valid.stdout)["payload"]["route_count"] == 1
    assert marker not in valid.stdout
    for command in ([], ["--check-config"]):
        entry["rate_limit"]["burst"] = marker
        invalid = subprocess.run([binary, *command],
                                 env=build_test_environment({"APIGATE_ROUTES": json.dumps([entry])}),
                                 capture_output=True, text=True, timeout=5, check=False)
        assert invalid.returncode != 0 and not invalid.stdout
        assert json.loads(invalid.stderr)["category"] == "configuration"
        assert marker not in invalid.stderr
    help_result = subprocess.run([binary, "--help"], capture_output=True, text=True,
                                 timeout=5, check=False)
    assert help_result.returncode == 0 and "rate_limit" in help_result.stdout


def test_route_rate_limits(binary):
    with ExitStack() as resources:
        upstream = ControlledUpstream()
        upstream.start()
        resources.callback(upstream.close)
        routes = [
            {"prefix": "/limited", "host": "127.0.0.1", "port": upstream.port,
             "strip_prefix": True, "rate_limit": {"requests_per_second": 1, "burst": 2}},
            {"prefix": "/other", "host": "127.0.0.1", "port": upstream.port,
             "strip_prefix": True, "rate_limit": {"requests_per_second": 1, "burst": 1}},
            {"prefix": "/limited/admin", "host": "127.0.0.1", "port": upstream.port,
             "rate_limit": {"requests_per_second": 1, "burst": 1}},
            {"prefix": "/bad", "host": "127.0.0.1", "port": upstream.port,
             "strip_prefix": True, "rate_limit": {"requests_per_second": 1, "burst": 1}},
            {"prefix": "/unlimited", "host": "127.0.0.1", "port": upstream.port},
            *[{"prefix": path, "host": "127.0.0.1", "port": upstream.port,
               "rate_limit": {"requests_per_second": 1, "burst": 1}}
              for path in ("/healthz", "/readyz")],
        ]
        service = ServiceProcess(binary, {"APIGATE_ROUTES": json.dumps(routes)})
        resources.callback(service.process.stdout.close)
        resources.callback(service.cleanup)
        port = int(service.wait_for_event("http_listener_started")["port"])
        client = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        resources.callback(client.close)
        second_client = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        resources.callback(second_client.close)
        marker = "rate-payload-" + secrets.token_hex(12)

        def healthy():
            before = upstream.state.request_count()
            for path in ("/healthz", "/readyz", "/healthz", "/readyz"):
                response, _ = read_json_response(client, "GET", path)
                assert response.status == 200
            assert upstream.state.request_count() == before

        def forwarded(path, expected, method="GET", body=None):
            response, payload = read_json_response(client, method, path, body=body)
            assert response.status == 201 and payload == {"upstream": "ok"}
            received = upstream.state.requests.get(timeout=3)
            assert received["path"] == expected and received["method"] == method
            assert received["host"] == f"127.0.0.1:{upstream.port}"
            assert received["body"] == (body or b"")
            if method != "GET":
                assert received["content_length"] == str(len(body or b""))

        def rejected(connection, path, method="GET", body=None):
            before = upstream.state.request_count()
            response, payload = read_json_response(connection, method, path, body=body)
            assert response.status == 429 and payload == {
                "error": {"code": "rate_limited", "message": "request rate limit is exceeded"}}
            retry_after = response.getheader("Retry-After")
            assert retry_after is not None and retry_after.isdecimal() and int(retry_after) >= 1
            assert upstream.state.request_count() == before
            return int(retry_after)

        healthy()
        # Invalid requests must not consume any of the first two tokens.
        for method, path, body, status in (
            ("DELETE", "/limited", None, 405), ("GET", "/limited", b"invalid-body", 400),
            ("POST", "/missing", None, 404),
        ):
            response, _ = read_json_response(client, method, path, body=body)
            assert response.status == status
        assert upstream.state.request_count() == 0
        forwarded("/limited?marker=" + marker, "/?marker=" + marker)
        original_socket = client.sock
        forwarded("/limited/42", "/42", "POST", marker.encode("ascii"))
        retry_after = rejected(client, "/limited/43?marker=" + marker,
                               "POST", marker.encode("ascii"))
        assert client.sock is original_socket
        # A new session shares the same route bucket, rather than receiving a fresh burst.
        rejected(second_client, "/limited/44")
        forwarded("/other/42", "/42")
        rejected(client, "/other/43")
        forwarded("/limited/admin/42", "/limited/admin/42")
        rejected(client, "/limited/admin/43")
        for index in range(3):
            path = f"/unlimited/{index}"
            forwarded(path, path)
        healthy()
        assert client.sock is original_socket

        # A real upstream protocol failure does not refund the already consumed token.
        before = upstream.state.request_count()
        response, _ = read_json_response(client, "GET", "/bad/informational-100")
        assert response.status == 502 and upstream.state.request_count() == before + 1
        assert upstream.state.requests.get(timeout=3)["path"] == "/informational-100"
        rejected(client, "/bad/informational-100")

        # Pace retries using the actual Retry-After suggestion, not an assumed refill sleep.
        # Each success/429 is observed; there is a single total recovery deadline.
        deadline = time.monotonic() + 5
        retry_wait = threading.Event()
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "route did not recover within the bounded retry window"
            retry_wait.wait(timeout=min(retry_after, remaining))
            before = upstream.state.request_count()
            response, payload = read_json_response(client, "POST", "/limited/recovered?" + marker,
                                                   body=marker.encode("ascii"))
            if response.status == 201:
                assert payload == {"upstream": "ok"}
                received = upstream.state.requests.get(timeout=3)
                assert received["path"] == "/recovered?" + marker
                assert received["body"] == marker.encode("ascii")
                assert upstream.state.request_count() == before + 1
                break
            assert response.status == 429 and payload["error"]["code"] == "rate_limited"
            assert upstream.state.request_count() == before
            retry_after = int(response.getheader("Retry-After"))
            assert retry_after >= 1
        healthy()
        assert client.sock is original_socket
        client.close()
        second_client.close()
        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        assert all(record["level"] not in ("error", "critical") for record in records)
        failures = [record["payload"] for record in records
                    if record["payload"].get("event") == "http_upstream_request_failed"]
        assert len(failures) == 1 and failures[0]["stage"] == "invalid_response"
        completions = [record["payload"] for record in records
                       if record["payload"].get("event") == "http_request_completed"]
        assert sum(payload.get("status") == 429 for payload in completions) >= 5
        for event in ("http_proxy_rejected_capacity", "http_buffer_budget_rejected",
                      "http_response_write_failed", "http_accept_retry_failed"):
            assert_event_absent(output, event)
        assert_sensitive_values_absent(output, {"query_and_body": marker, "route": "/limited"})
        assert service.process.poll() == 0


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: rate_limit_test.py <api-gate-binary>")
    binary = os.path.abspath(sys.argv[1])
    verify_configuration(binary)
    test_route_rate_limits(binary)
    print("route rate limits: burst, global clients, independent routes, recovery and keep-alive passed")


if __name__ == "__main__":
    main()
