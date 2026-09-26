#!/usr/bin/env python3

import hashlib
import http.client
import json
import os
import queue
import re
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timedelta


SUPPORTED_LOG_LEVELS = {"trace", "debug", "info", "warning", "error", "critical"}
UTC_TIMESTAMP_PATTERN = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$"
)


def safe_log_summary(line_number, line, reason):
    encoded_line = line.encode("utf-8", errors="replace")
    return {
        "line": line_number,
        "length": len(encoded_line),
        "sha256": hashlib.sha256(encoded_line).hexdigest()[:16],
        "reason": reason,
    }


def decode_json_log(line_number, line):
    try:
        record = json.loads(line)
    except json.JSONDecodeError:
        return None, safe_log_summary(line_number, line, "invalid_json")
    if not isinstance(record, dict):
        return None, safe_log_summary(line_number, line, "not_an_object")
    return record, None


def validate_normal_log(line_number, line):
    record, invalid_log = decode_json_log(line_number, line)
    if invalid_log is not None:
        return None, invalid_log

    timestamp = record.get("timestamp")
    if not isinstance(timestamp, str) or not UTC_TIMESTAMP_PATTERN.fullmatch(timestamp):
        return None, safe_log_summary(line_number, line, "invalid_utc_timestamp")
    try:
        parsed_timestamp = datetime.fromisoformat(timestamp[:-1] + "+00:00")
    except ValueError:
        return None, safe_log_summary(line_number, line, "invalid_utc_timestamp")
    if parsed_timestamp.utcoffset() != timedelta(0):
        return None, safe_log_summary(line_number, line, "invalid_utc_timestamp")

    if record.get("level") not in SUPPORTED_LOG_LEVELS:
        return None, safe_log_summary(line_number, line, "unsupported_level")

    payload = record.get("payload")
    if not isinstance(payload, dict):
        return None, safe_log_summary(line_number, line, "payload_not_an_object")
    for field in ("event", "service", "environment"):
        value = payload.get(field)
        if not isinstance(value, str) or not value:
            return None, safe_log_summary(line_number, line, f"invalid_payload_{field}")

    return record, None


def assert_json_log_output(output):
    invalid_logs = []
    for line_number, line in enumerate(output.splitlines(), start=1):
        _, invalid_log = decode_json_log(line_number, line)
        if invalid_log is not None:
            invalid_logs.append(invalid_log)
    assert not invalid_logs, f"invalid structured logs: {invalid_logs!r}"


def assert_normal_log_output(output):
    records = []
    invalid_logs = []
    for line_number, line in enumerate(output.splitlines(), start=1):
        if not line:
            continue
        record, invalid_log = validate_normal_log(line_number, line)
        if invalid_log is not None:
            invalid_logs.append(invalid_log)
        else:
            records.append(record)
    assert not invalid_logs, f"invalid normal service logs: {invalid_logs!r}"
    return records


def assert_sensitive_values_absent(output, sensitive_values):
    leaked_categories = [
        category for category, value in sensitive_values.items() if value in output
    ]
    assert not leaked_categories, (
        f"sensitive request data appeared in logs: {leaked_categories!r}"
    )


def parse_single_json_log(output):
    nonempty_lines = [line for line in output.splitlines() if line]
    assert len(nonempty_lines) == 1, (
        f"expected exactly one non-empty JSON log, got {len(nonempty_lines)}"
    )
    record, invalid_log = decode_json_log(1, nonempty_lines[0])
    assert invalid_log is None, f"invalid structured log: {invalid_log!r}"
    assert record is not None
    return record


class ServiceProcess:
    def __init__(self, binary, extra_env=None):
        environment = os.environ.copy()
        environment.update(
            {
                "APIGATE_LISTEN_ADDRESS": "127.0.0.1",
                "APIGATE_LISTEN_PORT": "0",
                "APIGATE_LOG_LEVEL": "info",
            }
        )
        if extra_env:
            environment.update(extra_env)
        self.process = subprocess.Popen(
            [binary],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=environment,
        )
        self.events = queue.Queue()
        self.lines = []
        self.invalid_logs = []
        self.reader = threading.Thread(target=self._read_logs, daemon=True)
        self.reader.start()

    def _read_logs(self):
        assert self.process.stdout is not None
        for line_number, line in enumerate(self.process.stdout, start=1):
            self.lines.append(line)
            if not line.strip():
                continue
            record, invalid_log = decode_json_log(line_number, line)
            if invalid_log is not None:
                self.invalid_logs.append(invalid_log)
                continue
            assert record is not None
            payload = record.get("payload", {})
            if not isinstance(payload, dict):
                self.invalid_logs.append(
                    safe_log_summary(line_number, line, "payload_not_an_object")
                )
                continue
            self.events.put(payload)

    def wait_for_event(self, event_name, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None and self.events.empty():
                break
            try:
                event = self.events.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if event.get("event") == event_name:
                return event
        raise AssertionError(
            f"event {event_name!r} was not observed; exit={self.process.poll()}; "
            f"log_count={len(self.lines)}; invalid_logs={self.invalid_logs!r}"
        )

    def finish_log_reader(self, timeout=1.0):
        self.reader.join(timeout=timeout)
        assert not self.reader.is_alive(), "log reader did not finish"
        assert not self.invalid_logs, f"invalid structured logs: {self.invalid_logs!r}"

    def terminate(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)

    def cleanup(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.reader.join(timeout=1)


def read_json_response(connection, method, path, body=None, headers=None):
    connection.request(method, path, body=body, headers=headers or {})
    response = connection.getresponse()
    payload = json.loads(response.read())
    return response, payload


def assert_peer_closed(connection_socket, timeout):
    connection_socket.settimeout(timeout)
    try:
        received = connection_socket.recv(1)
    except ConnectionError:
        return
    except socket.timeout as error:
        raise AssertionError("peer did not close the connection before the timeout") from error
    assert received == b"", f"expected EOF, received {received!r}"


def test_invalid_log_detection():
    private_marker = "credential-like-value-must-not-appear"
    _, invalid_log = decode_json_log(7, private_marker)
    assert invalid_log is not None
    assert invalid_log["line"] == 7
    assert invalid_log["reason"] == "invalid_json"
    assert private_marker not in repr(invalid_log)

    try:
        assert_json_log_output(private_marker)
    except AssertionError as error:
        assert private_marker not in str(error)
    else:
        raise AssertionError("invalid JSON log was accepted")

    _, invalid_log = validate_normal_log(8, private_marker)
    assert invalid_log is not None
    assert private_marker not in repr(invalid_log)

    try:
        assert_normal_log_output(private_marker)
    except AssertionError as error:
        assert private_marker not in str(error)
    else:
        raise AssertionError("invalid normal service log was accepted")

    try:
        assert_sensitive_values_absent(private_marker, {"test_category": private_marker})
    except AssertionError as error:
        assert private_marker not in str(error)
    else:
        raise AssertionError("sensitive value was not detected")


def test_command_line(binary, expected_version):
    expected_output = f"api-gate {expected_version}\n"

    version = subprocess.run(
        [binary, "--version"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )
    assert version.returncode == 0
    assert version.stdout == expected_output
    assert version.stdout.splitlines() == [f"api-gate {expected_version}"]
    assert not version.stderr

    invalid_environment = os.environ.copy()
    invalid_environment.update(
        {
            "APIGATE_SERVICE_NAME": "invalid/service",
            "APIGATE_ENVIRONMENT": "",
            "APIGATE_LOG_LEVEL": "verbose",
            "APIGATE_LISTEN_ADDRESS": "localhost",
            "APIGATE_LISTEN_PORT": "65536",
        }
    )
    invalid_config_version = subprocess.run(
        [binary, "--version"],
        env=invalid_environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )
    assert invalid_config_version.returncode == 0
    assert invalid_config_version.stdout == expected_output
    assert not invalid_config_version.stderr

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as holder:
        holder.bind(("127.0.0.1", 0))
        holder.listen(1)
        occupied_port = holder.getsockname()[1]
        occupied_environment = os.environ.copy()
        occupied_environment.update(
            {
                "APIGATE_LISTEN_ADDRESS": "127.0.0.1",
                "APIGATE_LISTEN_PORT": str(occupied_port),
            }
        )
        occupied_port_version = subprocess.run(
            [binary, "--version"],
            env=occupied_environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        assert occupied_port_version.returncode == 0
        assert occupied_port_version.stdout == expected_output
        assert not occupied_port_version.stderr

    help_result = subprocess.run(
        [binary, "--help"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )
    assert help_result.returncode == 0
    assert "--version" in help_result.stdout
    assert not help_result.stderr

    for arguments in (
        ["--unknown"],
        ["--version", "extra"],
        ["--version", "--help"],
    ):
        rejected = subprocess.run(
            [binary, *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        assert rejected.returncode != 0
        assert not rejected.stdout
        record = parse_single_json_log(rejected.stderr)
        assert record["level"] == "error"
        assert record["event"] == "bootstrap_failed"
        assert record["category"] == "arguments"


def verify_request_limit(port):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    first_socket = None
    try:
        for request_number in range(1, 101):
            response, payload = read_json_response(connection, "GET", "/healthz")
            assert response.status == 200
            assert payload["status"] == "ok"
            if request_number == 1:
                first_socket = connection.sock
                assert first_socket is not None
            if request_number < 100:
                assert not response.will_close
                assert connection.sock is first_socket
            else:
                assert response.will_close
                assert response.getheader("Connection", "").lower() == "close"
    finally:
        connection.close()


def verify_read_timeout(port):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as incomplete:
        incomplete.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n")
        started_at = time.monotonic()
        assert_peer_closed(incomplete, timeout=13)
        elapsed = time.monotonic() - started_at
        assert elapsed >= 8, f"incomplete request closed too early after {elapsed:.2f}s"


def test_http_and_shutdown(binary):
    service = ServiceProcess(binary)
    try:
        listener_event = service.wait_for_event("http_listener_started")
        port = int(listener_event["port"])
        assert 0 < port <= 65535

        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        health, health_body = read_json_response(connection, "GET", "/healthz")
        assert health.status == 200
        assert health_body["status"] == "ok"
        first_socket = connection.sock

        ready, ready_body = read_json_response(connection, "GET", "/readyz")
        assert ready.status == 200
        assert ready_body["status"] == "ready"
        assert connection.sock is first_socket

        missing, missing_body = read_json_response(connection, "GET", "/missing")
        assert missing.status == 404
        assert missing_body["error"]["code"] == "not_found"

        method, method_body = read_json_response(
            connection, "POST", "/healthz", body=b"", headers={"Content-Length": "0"}
        )
        assert method.status == 405
        assert method.getheader("Allow") == "GET"
        assert method_body["error"]["code"] == "method_not_allowed"
        connection.close()

        with socket.create_connection(("127.0.0.1", port), timeout=3) as oversized:
            oversized.sendall(
                b"POST /healthz HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Length: 65537\r\n"
                b"Connection: close\r\n\r\n"
            )
            status_line = oversized.makefile("rb").readline(4096)
            assert b" 413 " in status_line, status_line

        with socket.create_connection(("127.0.0.1", port), timeout=3) as oversized_headers:
            oversized_headers.sendall(
                b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Fill: "
                + (b"a" * 9000)
                + b"\r\nConnection: close\r\n\r\n"
            )
            status_line = oversized_headers.makefile("rb").readline(4096)
            assert b" 431 " in status_line, status_line

        with socket.create_connection(("127.0.0.1", port), timeout=3) as malformed:
            malformed.sendall(b"not-an-http-request\r\n\r\n")
            status_line = malformed.makefile("rb").readline(4096)
            assert not status_line or b" 400 " in status_line, status_line

        verify_request_limit(port)
        verify_read_timeout(port)

        service.terminate()
        assert service.process.wait(timeout=5) == 0
        service.finish_log_reader()
        service.wait_for_event("shutdown_signal_received")
        service.wait_for_event("http_listener_stopped")
        service.wait_for_event("service_stopped")
    finally:
        service.cleanup()


def test_active_keep_alive_shutdown(binary):
    service = ServiceProcess(binary)
    connection = None
    try:
        listener_event = service.wait_for_event("http_listener_started")
        port = int(listener_event["port"])
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        response, payload = read_json_response(connection, "GET", "/healthz")
        assert response.status == 200
        assert payload["status"] == "ok"
        assert not response.will_close
        active_socket = connection.sock
        assert active_socket is not None

        service.terminate()
        assert service.process.wait(timeout=5) == 0
        assert_peer_closed(active_socket, timeout=1)
        service.finish_log_reader()
        service.wait_for_event("shutdown_signal_received")
        service.wait_for_event("http_listener_stopped")
        service.wait_for_event("service_stopped")
    finally:
        if connection is not None:
            connection.close()
        service.cleanup()


def test_sensitive_request_data_not_logged(binary):
    service = ServiceProcess(binary)
    connection = None
    try:
        listener_event = service.wait_for_event("http_listener_started")
        port = int(listener_event["port"])
        assert 0 < port <= 65535

        query_marker = "query-" + secrets.token_hex(16)
        authorization_marker = "authorization-" + secrets.token_hex(16)
        cookie_marker = "cookie-" + secrets.token_hex(16)
        body_marker = "body-" + secrets.token_hex(16)
        query_parameter = "credential=" + query_marker
        request_body = "payload=" + body_marker

        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        health, health_body = read_json_response(
            connection,
            "GET",
            "/healthz?" + query_parameter,
            headers={
                "Authorization": "Bearer " + authorization_marker,
                "Cookie": "session=" + cookie_marker,
            },
        )
        assert health.status == 200
        assert health_body["status"] == "ok"

        method, method_body = read_json_response(
            connection,
            "POST",
            "/healthz",
            body=request_body,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        assert method.status == 405
        assert method.getheader("Allow") == "GET"
        assert method_body["error"]["code"] == "method_not_allowed"
        active_socket = connection.sock
        assert active_socket is not None

        service.terminate()
        assert service.process.wait(timeout=5) == 0
        assert_peer_closed(active_socket, timeout=1)
        connection.close()
        connection = None
        service.finish_log_reader()
        service.wait_for_event("shutdown_signal_received")
        service.wait_for_event("http_listener_stopped")
        service.wait_for_event("service_stopped")

        complete_output = "".join(service.lines)
        records = assert_normal_log_output(complete_output)
        request_events = [
            record
            for record in records
            if record["payload"]["event"] == "http_request_completed"
        ]
        assert len(request_events) >= 2
        assert_sensitive_values_absent(
            complete_output,
            {
                "query_value": query_marker,
                "complete_query_parameter": query_parameter,
                "authorization_header": authorization_marker,
                "cookie_header": cookie_marker,
                "body_value": body_marker,
                "request_body": request_body,
            },
        )
    finally:
        if connection is not None:
            connection.close()
        service.cleanup()


def test_port_conflict_and_config_check(binary):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as holder:
        holder.bind(("127.0.0.1", 0))
        holder.listen(1)
        port = holder.getsockname()[1]
        environment = os.environ.copy()
        environment.update(
            {
                "APIGATE_SERVICE_NAME": "config-check-test",
                "APIGATE_ENVIRONMENT": "integration",
                "APIGATE_LOG_LEVEL": "info",
                "APIGATE_LISTEN_ADDRESS": "127.0.0.1",
                "APIGATE_LISTEN_PORT": str(port),
            }
        )

        conflict = subprocess.run(
            [binary],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
        assert conflict.returncode != 0
        assert "http_server_start_failed" in conflict.stdout
        assert_normal_log_output(conflict.stdout)

        for log_level in ("trace", "debug", "info", "warn", "error", "critical"):
            check_environment = environment.copy()
            check_environment["APIGATE_LOG_LEVEL"] = log_level
            config_check = subprocess.run(
                [binary, "--check-config"],
                env=check_environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
                check=False,
            )
            assert config_check.returncode == 0
            assert not config_check.stderr
            assert "http_listener_started" not in config_check.stdout
            record = parse_single_json_log(config_check.stdout)
            assert isinstance(record.get("timestamp"), str)
            assert record["timestamp"].endswith("Z")
            assert record["level"] == "info"
            payload = record["payload"]
            assert payload["event"] == "configuration_valid"
            assert payload["service"] == "config-check-test"
            assert payload["environment"] == "integration"
            assert payload["log_level"] == log_level
            assert payload["listen_address"] == "127.0.0.1"
            assert payload["listen_port"] == port

        private_marker = "invalid-config-private-marker-20260926"
        invalid_environment = environment.copy()
        invalid_environment["APIGATE_SERVICE_NAME"] = private_marker + "/"
        invalid_config = subprocess.run(
            [binary, "--check-config"],
            env=invalid_environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        assert invalid_config.returncode != 0
        assert not invalid_config.stdout
        assert private_marker not in invalid_config.stderr
        assert "http_listener_started" not in invalid_config.stderr
        record = parse_single_json_log(invalid_config.stderr)
        assert record["level"] == "error"
        assert record["event"] == "bootstrap_failed"
        assert record["category"] == "configuration"
        assert isinstance(record.get("message"), str)
        assert record["message"]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: http_server_test.py <api-gate-binary> <expected-version>")
    binary = os.path.abspath(sys.argv[1])
    expected_version = sys.argv[2]
    test_invalid_log_detection()
    test_command_line(binary, expected_version)
    test_http_and_shutdown(binary)
    test_active_keep_alive_shutdown(binary)
    test_sensitive_request_data_not_logged(binary)
    test_port_conflict_and_config_check(binary)


if __name__ == "__main__":
    main()
