#!/usr/bin/env python3

import http.client
import http.server
import json
import os
import queue
import secrets
import signal
import socket
import subprocess
import sys
import threading

from http_server_test import (
    ServiceProcess,
    assert_event_absent,
    assert_new_connection_not_served,
    assert_normal_log_output,
    assert_peer_closed,
    assert_shutdown_sequence,
    assert_sensitive_values_absent,
    build_test_environment,
    read_json_response,
)


MAX_UPSTREAM_BODY = 1024 * 1024
MAX_DOWNSTREAM_BODY = 64 * 1024


class UpstreamState:
    def __init__(self):
        self.requests = queue.Queue()
        self.block_started = threading.Event()
        self.release_block = threading.Event()
        self._request_count = 0
        self._lock = threading.Lock()

    def record(self, handler, body):
        with self._lock:
            self._request_count += 1
        self.requests.put(
            {
                "method": handler.command,
                "path": handler.path,
                "body": body,
                "host": handler.headers.get("Host"),
                "content_type": handler.headers.get("Content-Type"),
                "content_length": handler.headers.get("Content-Length"),
                "transfer_encoding": handler.headers.get("Transfer-Encoding"),
                "authorization": handler.headers.get("Authorization"),
                "cookie": handler.headers.get("Cookie"),
                "removed": handler.headers.get("X-Remove"),
                "end_to_end": handler.headers.get("X-End-To-End"),
                "forwarded_for": handler.headers.get("X-Forwarded-For"),
            }
        )

    def request_count(self):
        with self._lock:
            return self._request_count


class ControlledUpstreamHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, unused_format, *unused_args):
        pass

    def _handle_request(self):
        state = self.server.state
        content_length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_length)
        state.record(self, body)
        route = self.path.split("?", 1)[0]
        if route == "/informational-100":
            self.close_connection = True
            self.connection.sendall(
                b"HTTP/1.1 100 Continue\r\n\r\n"
                b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"
            )
            return
        if route == "/informational-101":
            self.close_connection = True
            self.connection.sendall(
                b"HTTP/1.1 101 Switching Protocols\r\n"
                b"Connection: Upgrade\r\nUpgrade: test-protocol\r\n\r\n"
            )
            return
        if route == "/blocked":
            state.block_started.set()
            if not state.release_block.wait(timeout=10):
                return
            self._send_response(200, b'{"upstream":"released"}')
            return
        if route == "/large":
            self._send_response(200, b"x" * (MAX_UPSTREAM_BODY + 1))
            return
        self.send_response(201)
        self.send_header("Content-Type", "application/json")
        self.send_header("X-Upstream-Safe", "preserved")
        self.send_header("Connection", "X-Upstream-Hop, close")
        self.send_header("X-Upstream-Hop", "remove-me")
        self.send_header("Keep-Alive", "timeout=5")
        body = b'{"upstream":"ok"}'
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self._handle_request()

    def do_POST(self):
        self._handle_request()

    def do_PUT(self):
        self._handle_request()

    def do_PATCH(self):
        self._handle_request()

    def _send_response(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class ControlledUpstream:
    def __init__(self):
        self.state = UpstreamState()
        self.server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), ControlledUpstreamHandler
        )
        self.server.state = self.state
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server.server_address[1]

    def start(self):
        self.thread.start()

    def close(self):
        self.state.release_block.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "upstream server thread did not finish"


def proxy_environment(upstream_port, timeout_ms="1000", max_proxies=None):
    environment = {
        "APIGATE_UPSTREAM_HOST": "127.0.0.1",
        "APIGATE_UPSTREAM_PORT": str(upstream_port),
        "APIGATE_UPSTREAM_TIMEOUT_MS": timeout_ms,
    }
    if max_proxies is not None:
        environment["APIGATE_MAX_CONCURRENT_PROXIES"] = str(max_proxies)
    return environment


def stop_service(service):
    service.terminate()
    assert service.process.wait(timeout=5) == 0
    service.finish_log_reader()
    service.wait_for_event("service_stopped")


def get_json_response(connection):
    response = connection.getresponse()
    payload = json.loads(response.read())
    return response, payload


def send_chunked_json_request(connection, method, path, chunks, headers=None):
    connection.request(
        method,
        path,
        body=chunks,
        headers=headers or {},
        encode_chunked=True,
    )
    return get_json_response(connection)


def test_proxy_success_and_security(binary):
    upstream = ControlledUpstream()
    upstream.start()
    service = ServiceProcess(binary, proxy_environment(upstream.port))
    connection = None
    try:
        port = int(service.wait_for_event("http_listener_started")["port"])
        query_marker = "proxy-query-" + secrets.token_hex(16)
        authorization_marker = "proxy-authorization-" + secrets.token_hex(16)
        cookie_marker = "proxy-cookie-" + secrets.token_hex(16)
        query = "credential=" + query_marker
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        response, body = read_json_response(
            connection,
            "GET",
            "/resource?" + query,
            headers={
                "Authorization": "Bearer " + authorization_marker,
                "Cookie": "session=" + cookie_marker,
                "Connection": "X-Remove",
                "X-Remove": "remove-me",
                "X-End-To-End": "safe-value",
                "X-Forwarded-For": "untrusted-client-value",
            },
        )
        assert response.status == 201
        assert body == {"upstream": "ok"}
        assert response.getheader("Server") == "ApiGate"
        assert response.getheader("X-Upstream-Safe") == "preserved"
        assert response.getheader("X-Upstream-Hop") is None
        assert response.getheader("Keep-Alive") is None

        observed = upstream.state.requests.get(timeout=3)
        assert observed["path"] == "/resource?" + query
        assert observed["host"] == f"127.0.0.1:{upstream.port}"
        assert observed["authorization"] == "Bearer " + authorization_marker
        assert observed["cookie"] == "session=" + cookie_marker
        assert observed["removed"] is None
        assert observed["end_to_end"] == "safe-value"
        assert observed["forwarded_for"] is None
        assert upstream.state.request_count() == 1

        health, health_body = read_json_response(connection, "GET", "/healthz")
        ready, ready_body = read_json_response(connection, "GET", "/readyz")
        assert health.status == 200 and health_body["status"] == "ok"
        assert ready.status == 200 and ready_body["status"] == "ready"
        assert upstream.state.request_count() == 1

        method, method_body = read_json_response(connection, "POST", "/healthz", body=b"")
        assert method.status == 405
        assert method_body["error"]["code"] == "method_not_allowed"

        body_request, body_error = read_json_response(
            connection, "GET", "/resource", body=b"non-empty"
        )
        assert body_request.status == 400
        assert body_error["error"]["code"] == "unsupported_request"

        absolute, absolute_error = read_json_response(
            connection, "GET", "http://example.invalid/resource"
        )
        assert absolute.status == 400
        assert absolute_error["error"]["code"] == "unsupported_request"
        upgrade, upgrade_error = read_json_response(
            connection, "GET", "/resource", headers={"Upgrade": "websocket"}
        )
        assert upgrade.status == 400
        assert upgrade_error["error"]["code"] == "unsupported_request"
        unsupported, unsupported_error = read_json_response(
            connection, "DELETE", "/resource"
        )
        assert unsupported.status == 405
        assert unsupported.getheader("Allow") == "GET, POST, PUT, PATCH"
        assert unsupported_error["error"]["code"] == "method_not_allowed"
        assert upstream.state.request_count() == 1

        connection.close()
        connection = None
        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        proxy_events = [
            record
            for record in records
            if record["payload"].get("event") == "http_request_completed"
            and record["payload"].get("route") == "proxy"
        ]
        assert len(proxy_events) == 1
        assert proxy_events[0]["payload"]["status"] == 201
        assert_sensitive_values_absent(
            output,
            {
                "query": query_marker,
                "authorization": authorization_marker,
                "cookie": cookie_marker,
            },
        )
    finally:
        if connection is not None:
            connection.close()
        service.cleanup()
        upstream.close()


def test_body_methods_limits_and_expectation(binary):
    upstream = ControlledUpstream()
    upstream.start()
    service = ServiceProcess(binary, proxy_environment(upstream.port))
    connection = None
    body_marker = "proxy-body-" + secrets.token_hex(16)
    query_marker = "proxy-body-query-" + secrets.token_hex(16)
    authorization_marker = "proxy-body-authorization-" + secrets.token_hex(16)
    cookie_marker = "proxy-body-cookie-" + secrets.token_hex(16)
    try:
        port = int(service.wait_for_event("http_listener_started")["port"])
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)

        expected_methods = []
        for method in ("POST", "PUT", "PATCH"):
            for request_body in (b"", (method + "=" + body_marker).encode()):
                path = f"/body/{method.lower()}?credential={query_marker}"
                response, response_body = read_json_response(
                    connection,
                    method,
                    path,
                    body=request_body,
                    headers={
                        "Content-Type": "application/octet-stream",
                        "Authorization": "Bearer " + authorization_marker,
                        "Cookie": "session=" + cookie_marker,
                    },
                )
                assert response.status == 201
                assert response_body == {"upstream": "ok"}
                observed = upstream.state.requests.get(timeout=3)
                assert observed["method"] == method
                assert observed["path"] == path
                assert observed["body"] == request_body
                assert observed["content_type"] == "application/octet-stream"
                assert observed["content_length"] == str(len(request_body))
                assert observed["transfer_encoding"] is None
                assert observed["host"] == f"127.0.0.1:{upstream.port}"
                expected_methods.append(method)

        chunked_body = ("chunked=" + body_marker).encode()
        chunked, chunked_response = send_chunked_json_request(
            connection,
            "PATCH",
            "/body/chunked?credential=" + query_marker,
            [chunked_body[:7], chunked_body[7:]],
            headers={"Content-Type": "application/octet-stream"},
        )
        assert chunked.status == 201
        assert chunked_response == {"upstream": "ok"}
        observed = upstream.state.requests.get(timeout=3)
        assert observed["method"] == "PATCH"
        assert observed["body"] == chunked_body
        assert observed["content_length"] == str(len(chunked_body))
        assert observed["transfer_encoding"] is None
        expected_methods.append("PATCH")

        maximum_body = b"m" * MAX_DOWNSTREAM_BODY
        maximum, maximum_response = read_json_response(
            connection,
            "PUT",
            "/body/maximum",
            body=maximum_body,
            headers={"Content-Type": "application/octet-stream"},
        )
        assert maximum.status == 201
        assert maximum_response == {"upstream": "ok"}
        observed = upstream.state.requests.get(timeout=3)
        assert observed["method"] == "PUT"
        assert observed["body"] == maximum_body
        assert observed["content_length"] == str(MAX_DOWNSTREAM_BODY)
        expected_methods.append("PUT")

        before_oversized = upstream.state.request_count()
        connection.close()
        connection = None
        with socket.create_connection(("127.0.0.1", port), timeout=3) as oversized_client:
            oversized_client.sendall(
                b"POST /body/oversized HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/octet-stream\r\n"
                + f"Content-Length: {MAX_DOWNSTREAM_BODY + 1}\r\n".encode("ascii")
                + b"Connection: keep-alive\r\n\r\n"
            )
            oversized = http.client.HTTPResponse(oversized_client)
            oversized.begin()
            oversized_body = json.loads(oversized.read())
            assert oversized.status == 413
            assert oversized_body == {
                "error": {
                    "code": "payload_too_large",
                    "message": "request body is too large",
                }
            }
        assert upstream.state.request_count() == before_oversized

        chunked_oversized_connection = http.client.HTTPConnection(
            "127.0.0.1", port, timeout=3
        )
        try:
            chunked_oversized, chunked_oversized_body = send_chunked_json_request(
                chunked_oversized_connection,
                "PATCH",
                "/body/chunked-oversized",
                [
                    b"x" * (MAX_DOWNSTREAM_BODY // 2),
                    b"y" * (MAX_DOWNSTREAM_BODY // 2 + 1),
                ],
                headers={"Content-Type": "application/octet-stream"},
            )
            assert chunked_oversized.status == 413
            assert chunked_oversized_body["error"]["code"] == "payload_too_large"
        finally:
            chunked_oversized_connection.close()
        assert upstream.state.request_count() == before_oversized

        with socket.create_connection(("127.0.0.1", port), timeout=3) as expect_client:
            expect_client.sendall(
                b"POST /body/expect HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Length: 32\r\n"
                b"Expect: 100-continue\r\n"
                b"Connection: keep-alive\r\n\r\n"
            )
            expectation = http.client.HTTPResponse(expect_client)
            expectation.begin()
            expectation_body = json.loads(expectation.read())
            assert expectation.status == 417
            assert expectation_body == {
                "error": {
                    "code": "expectation_failed",
                    "message": "Expect is not supported",
                }
            }
            assert expectation.getheader("Content-Type") == "application/json"
            assert expectation.getheader("Cache-Control") == "no-store"
            assert expectation.getheader("Server") == "ApiGate"
            assert expectation.will_close
            assert_peer_closed(expect_client, timeout=1)
        assert upstream.state.request_count() == before_oversized

        with socket.create_connection(("127.0.0.1", port), timeout=3) as expect_client:
            expect_client.sendall(
                b"POST /body/repeated-expect HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Length: 32\r\n"
                b"Expect:\r\n"
                b"Expect: 100-continue\r\n"
                b"Connection: keep-alive\r\n\r\n"
            )
            expectation = http.client.HTTPResponse(expect_client)
            expectation.begin()
            expectation_body = json.loads(expectation.read())
            assert expectation.status == 417
            assert expectation_body == {
                "error": {
                    "code": "expectation_failed",
                    "message": "Expect is not supported",
                }
            }
            assert expectation.getheader("Content-Type") == "application/json"
            assert expectation.getheader("Cache-Control") == "no-store"
            assert expectation.getheader("Server") == "ApiGate"
            assert expectation.will_close
            assert_peer_closed(expect_client, timeout=1)
        assert upstream.state.request_count() == before_oversized

        health_connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        try:
            health, health_body = read_json_response(health_connection, "GET", "/healthz")
            ready, ready_body = read_json_response(health_connection, "GET", "/readyz")
            assert health.status == 200 and health_body["status"] == "ok"
            assert ready.status == 200 and ready_body["status"] == "ready"
        finally:
            health_connection.close()
        assert upstream.state.request_count() == before_oversized

        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        completed_methods = [
            record["payload"].get("method")
            for record in records
            if record["payload"].get("event") == "http_request_completed"
            and record["payload"].get("route") == "proxy"
        ]
        assert completed_methods == expected_methods
        assert_sensitive_values_absent(
            output,
            {
                "query": query_marker,
                "authorization": authorization_marker,
                "cookie": cookie_marker,
                "body": body_marker,
                "expect": "100-continue",
            },
        )
    finally:
        if connection is not None:
            connection.close()
        service.cleanup()
        upstream.close()


def test_proxy_failures_are_bounded_and_service_survives(binary):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as unavailable:
        unavailable.bind(("127.0.0.1", 0))
        unavailable_port = unavailable.getsockname()[1]
        service = ServiceProcess(binary, proxy_environment(unavailable_port, max_proxies=1))
        try:
            port = int(service.wait_for_event("http_listener_started")["port"])
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            failed, failed_body = read_json_response(
                connection, "POST", "/unavailable", body=b"bounded-request"
            )
            assert failed.status == 502
            assert failed_body["error"]["code"] == "bad_gateway"
            failed_again, failed_again_body = read_json_response(
                connection, "PUT", "/still-unavailable", body=b"bounded-request"
            )
            assert failed_again.status == 502
            assert failed_again_body["error"]["code"] == "bad_gateway"
            health, health_body = read_json_response(connection, "GET", "/healthz")
            assert health.status == 200 and health_body["status"] == "ok"
            connection.close()
            stop_service(service)
            assert_normal_log_output("".join(service.lines))
        finally:
            service.cleanup()

    upstream = ControlledUpstream()
    upstream.start()
    service = ServiceProcess(binary, proxy_environment(upstream.port, "100", max_proxies=1))
    try:
        port = int(service.wait_for_event("http_listener_started")["port"])
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        timed_out, timeout_body = read_json_response(
            connection, "POST", "/blocked", body=b"bounded-request"
        )
        assert upstream.state.block_started.is_set()
        assert timed_out.status == 504
        assert timeout_body["error"]["code"] == "gateway_timeout"
        upstream.state.release_block.set()

        too_large, large_body = read_json_response(
            connection, "PATCH", "/large", body=b"bounded-request"
        )
        assert too_large.status == 502
        assert large_body["error"]["code"] == "bad_gateway"
        recovered, recovered_body = read_json_response(
            connection, "PUT", "/after-failures", body=b"bounded-request"
        )
        assert recovered.status == 201
        assert recovered_body == {"upstream": "ok"}
        health, health_body = read_json_response(connection, "GET", "/healthz")
        assert health.status == 200 and health_body["status"] == "ok"
        assert upstream.state.request_count() == 3
        connection.close()
        stop_service(service)
        records = assert_normal_log_output("".join(service.lines))
        failure_stages = {
            record["payload"].get("stage")
            for record in records
            if record["payload"].get("event") == "http_upstream_request_failed"
        }
        assert "timeout" in failure_stages
        assert "response_too_large" in failure_stages
    finally:
        service.cleanup()
        upstream.close()


def test_informational_responses_are_rejected(binary):
    upstream = ControlledUpstream()
    upstream.start()
    service = ServiceProcess(binary, proxy_environment(upstream.port))
    marker = "informational-query-" + secrets.token_hex(16)
    try:
        port = int(service.wait_for_event("http_listener_started")["port"])
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        for route in ("/informational-100", "/informational-101"):
            before = upstream.state.request_count()
            response, body = read_json_response(
                connection, "GET", route + "?credential=" + marker
            )
            assert response.status == 502
            assert body["error"]["code"] == "bad_gateway"
            assert upstream.state.request_count() == before + 1

            health, health_body = read_json_response(connection, "GET", "/healthz")
            assert health.status == 200 and health_body["status"] == "ok"

        connection.close()
        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        invalid_responses = [
            record
            for record in records
            if record["payload"].get("event") == "http_upstream_request_failed"
            and record["payload"].get("stage") == "invalid_response"
        ]
        assert len(invalid_responses) == 2
        assert_sensitive_values_absent(output, {"query": marker})
    finally:
        service.cleanup()
        upstream.close()


def test_proxy_capacity_rejects_without_visiting_upstream(binary):
    upstream = ControlledUpstream()
    upstream.start()
    service = ServiceProcess(binary, proxy_environment(upstream.port, max_proxies=1))
    blocked_result = queue.Queue()
    query_marker = "proxy-capacity-query-" + secrets.token_hex(16)
    authorization_marker = "proxy-capacity-authorization-" + secrets.token_hex(16)
    cookie_marker = "proxy-capacity-cookie-" + secrets.token_hex(16)
    body_marker = "proxy-capacity-body-" + secrets.token_hex(16)
    blocked_client = None
    connection = None
    try:
        port = int(service.wait_for_event("http_listener_started")["port"])

        def request_blocked():
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            try:
                response, body = read_json_response(
                    connection, "POST", "/blocked", body=b"held-body"
                )
                blocked_result.put((response.status, body))
            except (ConnectionError, http.client.HTTPException, socket.timeout) as error:
                blocked_result.put(error)
            finally:
                connection.close()

        blocked_client = threading.Thread(target=request_blocked)
        blocked_client.start()
        assert upstream.state.block_started.wait(timeout=3)
        assert upstream.state.request_count() == 1

        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        for method in ("POST", "PUT", "PATCH"):
            rejected, rejected_body = read_json_response(
                connection,
                method,
                "/rejected?credential=" + query_marker,
                body="payload=" + body_marker,
                headers={
                    "Content-Type": "application/x-www-form-urlencoded",
                    "Authorization": "Bearer " + authorization_marker,
                    "Cookie": "session=" + cookie_marker,
                },
            )
            assert rejected.status == 503
            assert rejected_body == {
                "error": {
                    "code": "gateway_overloaded",
                    "message": "proxy capacity is exhausted",
                }
            }
            assert rejected.getheader("Content-Type") == "application/json"
            assert rejected.getheader("Cache-Control") == "no-store"
            assert rejected.getheader("Server") == "ApiGate"
            assert rejected.getheader("Retry-After") is None
            assert upstream.state.request_count() == 1

        health, health_body = read_json_response(connection, "GET", "/healthz")
        assert health.status == 200 and health_body["status"] == "ok"
        upstream.state.release_block.set()
        blocked_client.join(timeout=5)
        assert not blocked_client.is_alive(), "blocked proxy client did not finish"
        first_result = blocked_result.get(timeout=1)
        assert first_result == (200, {"upstream": "released"})

        accepted, accepted_body = read_json_response(
            connection, "PATCH", "/accepted", body=b"accepted-body"
        )
        assert accepted.status == 201
        assert accepted_body == {"upstream": "ok"}
        assert upstream.state.request_count() == 2
        connection.close()
        connection = None

        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        rejected_events = [
            record["payload"]
            for record in records
            if record["payload"].get("event") == "http_proxy_rejected_capacity"
        ]
        assert rejected_events == [
            {
                "event": "http_proxy_rejected_capacity",
                "service": "api-gate",
                "environment": "development",
                "active_proxies": 1,
                "max_proxies": 1,
                "status": 503,
            }
        ] * 3
        completed_503 = [
            record
            for record in records
            if record["payload"].get("event") == "http_request_completed"
            and record["payload"].get("route") == "proxy"
            and record["payload"].get("status") == 503
        ]
        assert [record["payload"]["method"] for record in completed_503] == [
            "POST",
            "PUT",
            "PATCH",
        ]
        assert_sensitive_values_absent(
            output,
            {
                "query": query_marker,
                "authorization": authorization_marker,
                "cookie": cookie_marker,
                "body": body_marker,
            },
        )
    finally:
        upstream.state.release_block.set()
        if connection is not None:
            connection.close()
        service.cleanup()
        upstream.close()
        if blocked_client is not None:
            blocked_client.join(timeout=5)
            assert not blocked_client.is_alive(), "blocked proxy client did not finish during cleanup"


def test_shutdown_drains_active_proxy(binary):
    for shutdown_signal in (signal.SIGTERM, signal.SIGINT):
        upstream = ControlledUpstream()
        upstream.start()
        environment = proxy_environment(upstream.port, "5000", max_proxies=1)
        environment["APIGATE_SHUTDOWN_GRACE_MS"] = "3000"
        service = ServiceProcess(binary, environment)
        client_done = queue.Queue()
        query_marker = "drain-query-" + secrets.token_hex(16)
        authorization_marker = "drain-authorization-" + secrets.token_hex(16)
        cookie_marker = "drain-cookie-" + secrets.token_hex(16)
        body_marker = "drain-body-" + secrets.token_hex(16)
        try:
            port = int(service.wait_for_event("http_listener_started")["port"])

            def request_blocked():
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                        request_body = ("payload=" + body_marker).encode("ascii")
                        client.sendall(
                            (
                                "PATCH /blocked?credential="
                                + query_marker
                                + " HTTP/1.1\r\nHost: localhost\r\n"
                                + "Authorization: Bearer "
                                + authorization_marker
                                + "\r\nCookie: session="
                                + cookie_marker
                                + "\r\nContent-Type: application/octet-stream\r\n"
                                + f"Content-Length: {len(request_body)}\r\n"
                                + "Connection: keep-alive\r\n\r\n"
                            ).encode("ascii")
                            + request_body
                        )
                        response = http.client.HTTPResponse(client)
                        response.begin()
                        body = json.loads(response.read())
                        will_close = response.will_close
                        assert_peer_closed(client, timeout=1)
                        client_done.put((response.status, body, will_close))
                except (ConnectionError, http.client.HTTPException, socket.timeout) as error:
                    client_done.put(error)

            client = threading.Thread(target=request_blocked)
            client.start()
            assert upstream.state.block_started.wait(timeout=3)
            service.process.send_signal(shutdown_signal)
            service.wait_for_event("shutdown_signal_received")
            started = service.wait_for_event("shutdown_drain_started")
            assert started["active_sessions"] == 1
            assert started["grace_ms"] == 3000
            service.wait_for_event("http_listener_stopped")

            assert_new_connection_not_served(port)

            upstream.state.release_block.set()
            client.join(timeout=5)
            assert not client.is_alive(), "proxy client thread did not finish"
            result = client_done.get(timeout=1)
            if isinstance(result, Exception):
                raise result
            assert result == (200, {"upstream": "released"}, True)
            assert service.process.wait(timeout=5) == 0
            service.finish_log_reader()
            service.wait_for_event("shutdown_drain_completed")
            service.wait_for_event("service_stopped")
            output = "".join(service.lines)
            records = assert_shutdown_sequence(output, "shutdown_drain_completed")
            assert_event_absent(output, "shutdown_drain_timed_out")
            assert_event_absent(output, "http_upstream_request_failed")
            assert_event_absent(output, "http_response_write_failed")
            assert_event_absent(output, "http_accept_retry_failed")
            stopped = [
                record
                for record in records
                if record["payload"].get("event") == "service_stopped"
            ]
            assert len(stopped) == 1
            assert stopped[0]["payload"]["exit_code"] == 0
            assert_sensitive_values_absent(
                output,
                {
                    "query": query_marker,
                    "authorization": authorization_marker,
                    "cookie": cookie_marker,
                    "body": body_marker,
                },
            )
        finally:
            upstream.state.release_block.set()
            service.cleanup()
            upstream.close()


def test_shutdown_deadline_forces_active_proxy(binary):
    upstream = ControlledUpstream()
    upstream.start()
    environment = proxy_environment(upstream.port, "5000", max_proxies=1)
    environment["APIGATE_SHUTDOWN_GRACE_MS"] = "250"
    service = ServiceProcess(binary, environment)
    client_done = queue.Queue()
    try:
        port = int(service.wait_for_event("http_listener_started")["port"])

        def request_blocked():
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                    client.sendall(
                        b"GET /blocked HTTP/1.1\r\nHost: localhost\r\n"
                        b"Connection: keep-alive\r\n\r\n"
                    )
                    response = http.client.HTTPResponse(client)
                    response.begin()
                    response.read()
                    client_done.put("response")
            except (ConnectionError, http.client.HTTPException, socket.timeout):
                client_done.put("closed")

        client = threading.Thread(target=request_blocked)
        client.start()
        assert upstream.state.block_started.wait(timeout=3)
        service.terminate()
        service.wait_for_event("shutdown_signal_received")
        started = service.wait_for_event("shutdown_drain_started")
        assert started["active_sessions"] == 1
        assert started["grace_ms"] == 250
        service.wait_for_event("http_listener_stopped")
        timed_out = service.wait_for_event("shutdown_drain_timed_out", timeout=3)
        assert timed_out["forced_sessions"] == 1
        assert service.process.wait(timeout=3) == 0
        client.join(timeout=3)
        assert not client.is_alive(), "forced proxy client did not finish"
        assert client_done.get(timeout=1) == "closed"
        service.finish_log_reader()
        service.wait_for_event("service_stopped")

        output = "".join(service.lines)
        records = assert_shutdown_sequence(output, "shutdown_drain_timed_out")
        assert_event_absent(output, "shutdown_drain_completed")
        assert_event_absent(output, "http_upstream_request_failed")
        assert_event_absent(output, "http_response_write_failed")
        assert_event_absent(output, "http_accept_retry_failed")
        timeout_events = [
            record
            for record in records
            if record["payload"].get("event") == "shutdown_drain_timed_out"
        ]
        assert len(timeout_events) == 1
        stopped = [
            record
            for record in records
            if record["payload"].get("event") == "service_stopped"
        ]
        assert len(stopped) == 1
        assert stopped[0]["payload"]["exit_code"] == 0
    finally:
        upstream.state.release_block.set()
        service.cleanup()
        upstream.close()


def test_config_check_hides_upstream_host(binary):
    private_host = "private-upstream-marker.invalid"
    environment = build_test_environment(
        {
            "APIGATE_UPSTREAM_HOST": private_host,
            "APIGATE_UPSTREAM_PORT": "8080",
            "APIGATE_UPSTREAM_TIMEOUT_MS": "4321",
        }
    )
    result = subprocess.run(
        [binary, "--check-config"],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )
    assert result.returncode == 0
    assert not result.stderr
    assert private_host not in result.stdout
    record = json.loads(result.stdout)
    assert record["payload"]["proxy_enabled"] is True
    assert record["payload"]["upstream_timeout_ms"] == 4321


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: http_proxy_test.py <api-gate-binary>")
    binary = os.path.abspath(sys.argv[1])
    test_config_check_hides_upstream_host(binary)
    test_proxy_success_and_security(binary)
    test_body_methods_limits_and_expectation(binary)
    test_proxy_failures_are_bounded_and_service_survives(binary)
    test_informational_responses_are_rejected(binary)
    test_proxy_capacity_rejects_without_visiting_upstream(binary)
    test_shutdown_drains_active_proxy(binary)
    test_shutdown_deadline_forces_active_proxy(binary)


if __name__ == "__main__":
    main()
