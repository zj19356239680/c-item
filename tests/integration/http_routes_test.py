#!/usr/bin/env python3

import http.client
import json
import os
import secrets
import subprocess
import sys
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


def verify_route_config_cli(binary):
    private_prefix = "/private-prefix-" + secrets.token_hex(8)
    private_host = "private-host-" + secrets.token_hex(8) + ".test"
    entry = {"prefix": private_prefix, "host": private_host, "port": 9001, "strip_prefix": True}
    raw = json.dumps([entry])
    checked = subprocess.run(
        [binary, "--check-config"], env=build_test_environment({"APIGATE_ROUTES": raw}),
        capture_output=True, text=True, timeout=5, check=False,
    )
    assert checked.returncode == 0 and not checked.stderr
    payload = json.loads(checked.stdout)["payload"]
    assert payload["proxy_enabled"] is True and payload["route_count"] == 1
    assert "upstream_timeout_ms" not in payload
    for marker in (private_host, private_prefix, raw):
        assert marker not in checked.stdout and marker not in checked.stderr

    # The same pre-listener validation must apply to normal startup, not only --check-config.
    invalid = json.dumps([{**entry, "port": False}])
    for arguments, environment in (
        ([], {"APIGATE_ROUTES": invalid}),
        (["--check-config"], {"APIGATE_ROUTES": raw, "APIGATE_UPSTREAM_TIMEOUT_MS": ""}),
    ):
        failed = subprocess.run([binary, *arguments], env=build_test_environment(environment),
                                capture_output=True, text=True, timeout=5, check=False)
        assert failed.returncode != 0 and not failed.stdout
        error = json.loads(failed.stderr)
        assert error["event"] == "bootstrap_failed" and error["category"] == "configuration"
        for marker in (private_host, private_prefix, raw, invalid):
            assert marker not in failed.stderr
        if arguments:
            assert "conflicts" in error["message"]


def test_multiple_routes(binary):
    with ExitStack() as resources:
        users = ControlledUpstream()
        users.start()
        resources.callback(users.close)
        users.state.identity = "users-service"
        orders = ControlledUpstream()
        orders.start()
        resources.callback(orders.close)
        orders.state.identity = "orders-service"
        routes = [
            {"prefix": "/users", "host": "127.0.0.1", "port": users.port},
            {"prefix": "/orders", "host": "127.0.0.1", "port": orders.port, "timeout_ms": 5000},
            {"prefix": "/users/admin", "host": "127.0.0.1", "port": orders.port, "strip_prefix": False},
            {"prefix": "/api/users", "host": "127.0.0.1", "port": users.port, "strip_prefix": True},
            {"prefix": "/api/users/admin", "host": "127.0.0.1", "port": orders.port},
            {"prefix": "/blocked", "host": "127.0.0.1", "port": users.port, "timeout_ms": 150},
            {"prefix": "/healthz", "host": "127.0.0.1", "port": orders.port},
            {"prefix": "/readyz", "host": "127.0.0.1", "port": users.port},
        ]
        service = ServiceProcess(binary, {"APIGATE_ROUTES": json.dumps(routes)})
        resources.callback(service.process.stdout.close)
        resources.callback(service.cleanup)
        port = int(service.wait_for_event("http_listener_started")["port"])
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        resources.callback(connection.close)
        query = "route-query-" + secrets.token_hex(16)
        body_marker = "route-body-" + secrets.token_hex(16)
        original_socket = None

        def forward(method, target, upstream, body=None, expected_target=None):
            nonlocal original_socket
            response, payload = read_json_response(
                connection, method, target, body=body, headers={"Host": "untrusted.invalid:1234"}
            )
            assert response.status == 201 and payload == {"upstream": upstream.state.identity}
            if original_socket is None:
                original_socket = connection.sock
            assert original_socket is not None and connection.sock is original_socket
            received = upstream.state.requests.get(timeout=3)
            assert received["method"] == method
            assert received["path"] == (target if expected_target is None else expected_target)
            assert received["host"] == f"127.0.0.1:{upstream.port}"
            assert received["body"] == (body or b"")
            if method != "GET":
                assert received["content_length"] == str(len(body or b""))

        for target, upstream in (
            ("/users", users), ("/orders/42?marker=" + query, orders),
            ("/users/admin/7?literal=%2F&marker=" + query, orders),
            ("/users/42?literal=%2f", users),
        ):
            forward("GET", target, upstream)
        forward("POST", "/orders?marker=" + query, orders, body_marker.encode("ascii"))
        for target, expected in (
            ("/api/users/42?x=%2F&x=2&empty=&x=%2f", "/42?x=%2F&x=2&empty=&x=%2f"),
            ("/api/users", "/"), ("/api/users/", "/"),
            ("/api/users?", "/?"), ("/api/users?x=1", "/?x=1"),
            ("/api/users/healthz", "/healthz"), ("/api/users/readyz", "/readyz"),
            # The rewritten /orders must stay on users, not trigger another selection.
            ("/api/users/orders", "/orders"),
        ):
            forward("GET", target, users, expected_target=expected)
        forward("POST", "/api/users/42?marker=" + query, users,
                body_marker.encode("ascii"), expected_target="/42?marker=" + query)
        forward("GET", "/api/users/admin/42?x=%2f", orders)
        forward("GET", "/orders/unchanged?", orders)
        before = (users.state.request_count(), orders.state.request_count())
        for method, path in (("GET", "/users-extra"), ("POST", "/missing"), ("GET", "/Users")):
            response, payload = read_json_response(connection, method, path)
            assert response.status == 404 and payload["error"]["code"] == "not_found"
        for path in ("/healthz?marker=" + query, "/readyz"):
            response, _ = read_json_response(connection, "GET", path)
            assert response.status == 200
        assert before == (users.state.request_count(), orders.state.request_count())
        assert connection.sock is original_socket

        # A route-local 150ms deadline must win well before the legacy 3000ms default.
        connection.timeout = 1.5
        assert connection.sock is not None
        connection.sock.settimeout(1.5)
        response, payload = read_json_response(connection, "GET", "/blocked")
        assert users.state.block_started.wait(timeout=1)
        assert response.status == 504 and payload == {
            "error": {"code": "gateway_timeout", "message": "upstream request timed out"}
        }
        blocked_request = users.state.requests.get(timeout=1)
        assert blocked_request["path"] == "/blocked"
        assert not users.state.release_block.is_set()
        forward("GET", "/orders/after-timeout", orders)
        users.state.release_block.set()
        connection.close()
        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        assert all(record["level"] not in ("error", "critical") for record in records)
        failures = [record["payload"] for record in records
                    if record["payload"]["event"] == "http_upstream_request_failed"]
        assert len(failures) == 1 and failures[0]["stage"] == "timeout" and failures[0]["status"] == 504
        for event in ("http_response_write_failed", "http_accept_retry_failed", "http_proxy_rejected_capacity"):
            assert_event_absent(output, event)
        assert_sensitive_values_absent(output, {
            "query": query, "body": body_marker, "prefix": "/users",
            "users_response": users.state.identity, "orders_response": orders.state.identity,
        })
        service.finish_log_reader()
        assert service.process.poll() == 0
        service.process.stdout.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: http_routes_test.py <api-gate-binary>")
    binary = os.path.abspath(sys.argv[1])
    verify_route_config_cli(binary)
    test_multiple_routes(binary)
    print("static routes: two upstreams, optional prefix stripping, keep-alive, Host, raw query and route timeout passed")


if __name__ == "__main__":
    main()
