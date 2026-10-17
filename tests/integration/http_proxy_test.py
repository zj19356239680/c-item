#!/usr/bin/env python3

import http.client
import http.server
import json
import os
import queue
import secrets
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

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
        self.known_header_sent = threading.Event()
        self.release_known_body = threading.Event()
        self.chunked_header_sent = threading.Event()
        self.release_chunked_body = threading.Event()
        self.blocked_body_started = threading.Event()
        self.release_blocked_body = threading.Event()
        self.response_marker = "response-marker-not-configured"
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
        if route == "/blocked-no-content":
            state.block_started.set()
            if not state.release_block.wait(timeout=10):
                return
            self._send_no_content()
            return
        if route == "/no-content":
            self._send_no_content()
            return
        if route == "/budget-known":
            body = (state.response_marker + "-" + "x" * 128).encode("ascii")
            self.close_connection = True
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                + f"Content-Length: {len(body)}\r\n".encode("ascii")
                + b"Connection: close\r\n\r\n"
            )
            state.known_header_sent.set()
            if state.release_known_body.wait(timeout=10):
                try:
                    self.connection.sendall(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            return
        if route == "/budget-chunked":
            body = (state.response_marker + "-chunked").encode("ascii")
            self.close_connection = True
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
            )
            state.chunked_header_sent.set()
            if state.release_chunked_body.wait(timeout=10):
                try:
                    self.connection.sendall(
                        f"{len(body):X}\r\n".encode("ascii")
                        + body
                        + b"\r\n0\r\n\r\n"
                    )
                except (BrokenPipeError, ConnectionResetError):
                    pass
            return
        if route == "/blocked-response-body":
            body = b'{"held":"body"}'
            self.close_connection = True
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                + f"Content-Length: {len(body)}\r\n".encode("ascii")
                + b"Connection: close\r\n\r\n"
            )
            state.blocked_body_started.set()
            if state.release_blocked_body.wait(timeout=10):
                try:
                    self.connection.sendall(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            return
        if route == "/large":
            self._send_response(200, b"x" * (MAX_UPSTREAM_BODY + 1))
            return
        if route == "/write-full":
            self.connection.settimeout(3)
            body = state.response_marker.encode("ascii").ljust(MAX_UPSTREAM_BODY, b"w")
            self._send_response(200, body)
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

    def _send_no_content(self):
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()


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
        self.state.release_known_body.set()
        self.state.release_chunked_body.set()
        self.state.release_blocked_body.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "upstream server thread did not finish"


def proxy_environment(
    upstream_port, timeout_ms="1000", max_proxies=None, max_in_flight_bytes=None
):
    environment = {
        "APIGATE_UPSTREAM_HOST": "127.0.0.1",
        "APIGATE_UPSTREAM_PORT": str(upstream_port),
        "APIGATE_UPSTREAM_TIMEOUT_MS": timeout_ms,
    }
    if max_proxies is not None:
        environment["APIGATE_MAX_CONCURRENT_PROXIES"] = str(max_proxies)
    if max_in_flight_bytes is not None:
        environment["APIGATE_MAX_IN_FLIGHT_BYTES"] = str(max_in_flight_bytes)
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
    service = ServiceProcess(
        binary,
        proxy_environment(
            upstream.port, "100", max_proxies=1, max_in_flight_bytes=40
        ),
    )
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

        body_timed_out, body_timeout_payload = read_json_response(
            connection, "GET", "/blocked-response-body"
        )
        assert upstream.state.blocked_body_started.is_set()
        assert body_timed_out.status == 504
        assert body_timeout_payload["error"]["code"] == "gateway_timeout"
        upstream.state.release_blocked_body.set()

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
        assert upstream.state.request_count() == 4
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
        upstream.state.release_block.set()
        upstream.state.release_blocked_body.set()
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


def test_global_buffer_budget(binary):
    upstream = ControlledUpstream()
    upstream.state.response_marker = "budget-response-" + secrets.token_hex(16)
    upstream.start()
    sensitive = {
        "query": "budget-query-" + secrets.token_hex(16),
        "authorization": "budget-authorization-" + secrets.token_hex(16),
        "cookie": "budget-cookie-" + secrets.token_hex(16),
        "request_body": "budget-request-body-" + secrets.token_hex(16),
        "response_body": upstream.state.response_marker,
    }
    outputs = []
    try:
        service = ServiceProcess(
            binary,
            proxy_environment(upstream.port, max_in_flight_bytes=16),
        )
        try:
            port = int(service.wait_for_event("http_listener_started")["port"])
            before = upstream.state.request_count()
            with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                client.sendall(
                    (
                        "POST /known-rejected?credential="
                        + sensitive["query"]
                        + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer "
                        + sensitive["authorization"]
                        + "\r\nCookie: session="
                        + sensitive["cookie"]
                        + "\r\nContent-Length: 17\r\nConnection: keep-alive\r\n\r\n"
                    ).encode("ascii")
                )
                response = http.client.HTTPResponse(client)
                response.begin()
                body = json.loads(response.read())
                assert response.status == 503
                assert body == {
                    "error": {
                        "code": "gateway_overloaded",
                        "message": "buffer capacity is exhausted",
                    }
                }
                assert response.getheader("Retry-After") is None
                assert response.will_close
                assert_peer_closed(client, timeout=1)
            assert upstream.state.request_count() == before

            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            connection.request("POST", "/no-content", body=b"0123456789abcdef")
            accepted = connection.getresponse()
            assert accepted.status == 204
            assert accepted.read() == b""
            connection.close()
            assert upstream.state.request_count() == before + 1
            stop_service(service)
            output = "".join(service.lines)
            outputs.append(output)
            events = [
                record["payload"]
                for record in assert_normal_log_output(output)
                if record["payload"].get("event") == "http_buffer_budget_rejected"
            ]
            assert len(events) == 1
            assert events[0]["direction"] == "request"
            assert events[0]["requested_bytes"] == 17
            assert events[0]["active_bytes"] == 0
            assert events[0]["max_bytes"] == 16
            assert events[0]["status"] == 503
        finally:
            service.cleanup()

        rejected_chunked = ServiceProcess(
            binary,
            proxy_environment(upstream.port, max_in_flight_bytes=MAX_DOWNSTREAM_BODY - 1),
        )
        try:
            port = int(rejected_chunked.wait_for_event("http_listener_started")["port"])
            before = upstream.state.request_count()
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            response, body = send_chunked_json_request(
                connection,
                "PATCH",
                "/chunked-rejected?credential=" + sensitive["query"],
                [sensitive["request_body"].encode("ascii")],
                headers={
                    "Authorization": "Bearer " + sensitive["authorization"],
                    "Cookie": "session=" + sensitive["cookie"],
                },
            )
            assert response.status == 503
            assert body["error"]["message"] == "buffer capacity is exhausted"
            assert response.will_close
            connection.close()
            assert upstream.state.request_count() == before
            stop_service(rejected_chunked)
            output = "".join(rejected_chunked.lines)
            outputs.append(output)
            events = [
                record["payload"]
                for record in assert_normal_log_output(output)
                if record["payload"].get("event") == "http_buffer_budget_rejected"
            ]
            assert len(events) == 1
            assert events[0]["direction"] == "request"
            assert events[0]["requested_bytes"] == MAX_DOWNSTREAM_BODY
        finally:
            rejected_chunked.cleanup()

        accepted_chunked = ServiceProcess(
            binary,
            proxy_environment(upstream.port, max_in_flight_bytes=MAX_DOWNSTREAM_BODY),
        )
        try:
            port = int(accepted_chunked.wait_for_event("http_listener_started")["port"])
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            for suffix in ("first", "second"):
                connection.request(
                    "PATCH",
                    "/no-content?case=" + suffix,
                    body=[b"small", b"-chunked"],
                    encode_chunked=True,
                )
                response = connection.getresponse()
                assert response.status == 204
                assert response.read() == b""
            connection.close()
            stop_service(accepted_chunked)
            output = "".join(accepted_chunked.lines)
            outputs.append(output)
            assert_event_absent(output, "http_buffer_budget_rejected")
        finally:
            accepted_chunked.cleanup()

        known_response = ServiceProcess(
            binary,
            proxy_environment(upstream.port, max_in_flight_bytes=64),
        )
        try:
            port = int(known_response.wait_for_event("http_listener_started")["port"])
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            rejected, body = read_json_response(
                connection,
                "GET",
                "/budget-known?credential=" + sensitive["query"],
                headers={
                    "Authorization": "Bearer " + sensitive["authorization"],
                    "Cookie": "session=" + sensitive["cookie"],
                },
            )
            assert upstream.state.known_header_sent.is_set()
            assert rejected.status == 503
            assert body["error"]["message"] == "buffer capacity is exhausted"
            upstream.state.release_known_body.set()
            health, health_body = read_json_response(connection, "GET", "/healthz")
            assert health.status == 200 and health_body["status"] == "ok"
            connection.close()
            stop_service(known_response)
            output = "".join(known_response.lines)
            outputs.append(output)
            records = assert_normal_log_output(output)
            events = [
                record["payload"]
                for record in records
                if record["payload"].get("event") == "http_buffer_budget_rejected"
            ]
            assert len(events) == 1
            assert events[0]["direction"] == "response"
            assert events[0]["requested_bytes"] > 64
            assert_event_absent(output, "http_upstream_request_failed")
        finally:
            upstream.state.release_known_body.set()
            known_response.cleanup()

        chunked_response = ServiceProcess(
            binary,
            proxy_environment(upstream.port, max_in_flight_bytes=MAX_UPSTREAM_BODY - 1),
        )
        try:
            port = int(chunked_response.wait_for_event("http_listener_started")["port"])
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            rejected, body = read_json_response(connection, "GET", "/budget-chunked")
            assert upstream.state.chunked_header_sent.is_set()
            assert rejected.status == 503
            assert body["error"]["message"] == "buffer capacity is exhausted"
            upstream.state.release_chunked_body.set()
            health, health_body = read_json_response(connection, "GET", "/healthz")
            assert health.status == 200 and health_body["status"] == "ok"
            connection.close()
            stop_service(chunked_response)
            output = "".join(chunked_response.lines)
            outputs.append(output)
            records = assert_normal_log_output(output)
            events = [
                record["payload"]
                for record in records
                if record["payload"].get("event") == "http_buffer_budget_rejected"
            ]
            assert len(events) == 1
            assert events[0]["direction"] == "response"
            assert events[0]["requested_bytes"] == MAX_UPSTREAM_BODY
            assert_event_absent(output, "http_upstream_request_failed")
        finally:
            upstream.state.release_chunked_body.set()
            chunked_response.cleanup()

        shared_service = ServiceProcess(
            binary,
            proxy_environment(upstream.port, max_in_flight_bytes=48),
        )
        first_result = queue.Queue()
        first_client = None
        try:
            port = int(shared_service.wait_for_event("http_listener_started")["port"])

            def hold_global_budget():
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                try:
                    connection.request("POST", "/blocked-no-content", body=b"a" * 32)
                    response = connection.getresponse()
                    first_result.put((response.status, response.read()))
                except (ConnectionError, http.client.HTTPException, socket.timeout) as error:
                    first_result.put(error)
                finally:
                    connection.close()

            first_client = threading.Thread(target=hold_global_budget)
            first_client.start()
            assert upstream.state.block_started.wait(timeout=3)
            before = upstream.state.request_count()

            second = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            rejected, body = read_json_response(
                second, "POST", "/global-rejected", body=b"b" * 17
            )
            assert rejected.status == 503
            assert body["error"]["message"] == "buffer capacity is exhausted"
            second.close()
            assert upstream.state.request_count() == before

            upstream.state.release_block.set()
            first_client.join(timeout=5)
            assert not first_client.is_alive(), "budget holder did not finish"
            assert first_result.get(timeout=1) == (204, b"")

            recovered = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            recovered.request("POST", "/no-content", body=b"b" * 17)
            recovered_response = recovered.getresponse()
            assert recovered_response.status == 204
            assert recovered_response.read() == b""
            recovered.close()
            stop_service(shared_service)
            output = "".join(shared_service.lines)
            outputs.append(output)
            records = assert_normal_log_output(output)
            events = [
                record["payload"]
                for record in records
                if record["payload"].get("event") == "http_buffer_budget_rejected"
            ]
            assert len(events) == 1
            assert events[0]["direction"] == "request"
            assert events[0]["requested_bytes"] == 17
            assert events[0]["active_bytes"] == 32
            assert events[0]["max_bytes"] == 48
        finally:
            upstream.state.release_block.set()
            shared_service.cleanup()
            if first_client is not None:
                first_client.join(timeout=5)
                assert not first_client.is_alive(), "budget holder did not finish during cleanup"

        combined_output = "".join(outputs)
        assert_sensitive_values_absent(combined_output, sensitive)
    finally:
        upstream.close()


def test_downstream_reset_recovers_full_response_budget(binary):
    upstream = ControlledUpstream()
    upstream.state.response_marker = "write-response-" + secrets.token_hex(16)
    expected = upstream.state.response_marker.encode("ascii").ljust(MAX_UPSTREAM_BODY, b"w")
    upstream.start()
    service = None
    try:
        service = ServiceProcess(
            binary,
            proxy_environment(upstream.port, max_proxies=16, max_in_flight_bytes=MAX_UPSTREAM_BODY),
        )
        port = int(service.wait_for_event("http_listener_started")["port"])
        request_count = 0
        for _ in range(3):
            # A real process reset regression. Header receipt proves the proxy has
            # buffered the upstream response, but does NOT prove async_write is
            # pending: that stronger property is tested by SessionWriteRegressionTest.
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
                client.settimeout(3)
                client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                client.connect(("127.0.0.1", port))
                client.sendall(
                    b"GET /write-full HTTP/1.1\r\nHost: localhost\r\n"
                    b"Connection: close\r\n\r\n"
                )
                request_count += 1
                with http.client.HTTPResponse(client) as response:
                    response.begin()
                    assert response.status == 200
                    assert int(response.getheader("Content-Length")) == MAX_UPSTREAM_BODY
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))

            deadline = time.monotonic() + 5
            while True:
                remaining = deadline - time.monotonic()
                assert remaining > 0, "full response budget was not reusable after reset"
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=remaining)
                try:
                    connection.request("GET", "/write-full")
                    recovered = connection.getresponse()
                    body = recovered.read()
                    request_count += 1
                    if recovered.status == 200:
                        assert body == expected, "recovered upstream response content differed"
                        break
                    assert recovered.status == 503
                    assert json.loads(body) == {
                        "error": {
                            "code": "gateway_overloaded",
                            "message": "buffer capacity is exhausted",
                        }
                    }
                finally:
                    connection.close()

            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                health, body = read_json_response(connection, "GET", "/healthz")
                request_count += 1
                assert health.status == 200 and body["status"] == "ok"
            finally:
                connection.close()

        stop_service(service)
        output = "".join(service.lines)
        records = assert_normal_log_output(output)
        assert all(record["level"] not in ("error", "critical") for record in records)
        for event in (
            "http_proxy_rejected_capacity",
            "http_upstream_request_failed",
            "http_response_write_failed",
            "http_accept_retry_failed",
        ):
            assert_event_absent(output, event)
        completions = [
            record for record in records
            if record["payload"].get("event") == "http_request_completed"
        ]
        assert len(completions) == request_count, "request completion count differed"
        assert_sensitive_values_absent(output, {"response_body": upstream.state.response_marker})
    finally:
        try:
            if service is not None:
                service.cleanup()
        finally:
            upstream.close()


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
            assert_event_absent(output, "shutdown_force_requested")
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
        assert_event_absent(output, "shutdown_force_requested")
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


def test_second_signal_forces_active_proxy(binary):
    for first_signal, second_signal in (
        (signal.SIGTERM, signal.SIGTERM),
        (signal.SIGTERM, signal.SIGINT),
    ):
        upstream = ControlledUpstream()
        upstream.start()
        environment = proxy_environment(upstream.port, "15000", max_proxies=1)
        environment["APIGATE_SHUTDOWN_GRACE_MS"] = "10000"
        service = ServiceProcess(binary, environment)
        client_done = queue.Queue()
        query_marker = "force-query-" + secrets.token_hex(16)
        authorization_marker = "force-authorization-" + secrets.token_hex(16)
        cookie_marker = "force-cookie-" + secrets.token_hex(16)
        body_marker = "force-body-" + secrets.token_hex(16)
        client = None
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
                                + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer "
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
                        response.read()
                        client_done.put("response")
                except (ConnectionError, http.client.HTTPException, socket.timeout):
                    client_done.put("closed")

            client = threading.Thread(target=request_blocked)
            client.start()
            assert upstream.state.block_started.wait(timeout=3)
            service.process.send_signal(first_signal)
            service.wait_for_event("shutdown_signal_received")
            started = service.wait_for_event("shutdown_drain_started")
            assert started["active_sessions"] == 1
            assert started["grace_ms"] == 10000
            service.wait_for_event("http_listener_stopped")

            force_started_at = time.monotonic()
            service.process.send_signal(second_signal)
            forced = service.wait_for_event("shutdown_force_requested")
            assert forced["signal"] == second_signal
            assert forced["active_sessions"] == 1
            assert service.process.wait(timeout=5) == 0
            assert time.monotonic() - force_started_at < 5
            client.join(timeout=3)
            assert not client.is_alive(), "forced proxy client did not finish"
            assert client_done.get(timeout=1) == "closed"
            service.finish_log_reader()
            service.wait_for_event("service_stopped")

            output = "".join(service.lines)
            records = assert_shutdown_sequence(output, "shutdown_force_requested")
            assert_event_absent(output, "shutdown_drain_completed")
            assert_event_absent(output, "shutdown_drain_timed_out")
            assert_event_absent(output, "http_upstream_request_failed")
            assert_event_absent(output, "http_response_write_failed")
            assert_event_absent(output, "http_accept_retry_failed")
            force_events = [
                record
                for record in records
                if record["payload"].get("event") == "shutdown_force_requested"
            ]
            assert len(force_events) == 1
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
            if client is not None:
                client.join(timeout=5)
                assert not client.is_alive(), (
                    "forced proxy client did not finish during cleanup"
                )


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
    test_global_buffer_budget(binary)
    test_downstream_reset_recovers_full_response_budget(binary)
    test_shutdown_drains_active_proxy(binary)
    test_shutdown_deadline_forces_active_proxy(binary)
    test_second_signal_forces_active_proxy(binary)


if __name__ == "__main__":
    main()
