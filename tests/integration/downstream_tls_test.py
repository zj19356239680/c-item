#!/usr/bin/env python3
"""Verified downstream TLS, single-listener capacity, deadlines and cancellation."""
from contextlib import ExitStack
import http.client
import json
import os
from pathlib import Path
import select
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

from https_proxy_test import certificate_files
from http_server_test import (ServiceProcess, build_test_environment, read_json_response,
                              assert_normal_log_output, assert_sensitive_values_absent, assert_peer_closed)
from http_proxy_test import stop_service
from metrics_test import scrape, cumulative, failures
from upstream_pool_test import PoolBackend


def context(root, protocols=("http/1.1",), ca="ca.pem"):
    result = ssl.create_default_context(cafile=str(root / ca))
    result.minimum_version = ssl.TLSVersion.TLSv1_2
    if protocols is not None:
        result.set_alpn_protocols(list(protocols))
    return result


def start(resources, binary, root, **extra):
    service = ServiceProcess(binary, {"APIGATE_TLS_CERT_FILE": str(root / "server.pem"),
        "APIGATE_TLS_KEY_FILE": str(root / "server.key"), "APIGATE_METRICS_ENABLED": "true", **extra})
    resources.callback(service.process.stdout.close)
    resources.callback(service.cleanup)
    port = int(service.wait_for_event("http_listener_started")["port"])

    def client(client_context=None):
        connection = http.client.HTTPSConnection("127.0.0.1", port, timeout=4,
                                                 context=client_context or context(root))
        resources.callback(connection.close)
        return connection
    return service, port, client


def verified_peer(resources, root, port, protocols=("http/1.1",)):
    raw = socket.create_connection(("127.0.0.1", port), timeout=3)
    resources.callback(raw.close)
    peer = context(root, protocols).wrap_socket(raw, server_hostname="localhost")
    peer.settimeout(4)
    resources.callback(peer.close)
    return peer


def logs(service, markers=None):
    records = assert_normal_log_output("".join(service.lines))
    assert all(record["level"] not in ("error", "critical") for record in records)
    assert_sensitive_values_absent("".join(service.lines), markers or {})
    return [record["payload"] for record in records]


def run(binary, arguments, environment):
    return subprocess.run([binary, *arguments], env=build_test_environment(environment),
                          capture_output=True, timeout=4, check=False)


def test_configuration(binary, root):
    encrypted = root / "encrypted.key"
    result = subprocess.run(["openssl", "pkey", "-in", str(root / "server.key"), "-aes-256-cbc",
                             "-passout", "pass:disposable-test-only", "-out", str(encrypted)],
                            capture_output=True, timeout=5, check=False)
    assert result.returncode == 0, "test encrypted key generation failed"
    invalid = root / "invalid.pem"
    invalid.write_text("private-invalid-pem-marker", encoding="utf-8")
    cert, key = str(root / "server.pem"), str(root / "server.key")
    for environment in ({"APIGATE_TLS_CERT_FILE": cert}, {"APIGATE_TLS_KEY_FILE": key},
            {"APIGATE_TLS_CERT_FILE": "", "APIGATE_TLS_KEY_FILE": key},
            {"APIGATE_TLS_CERT_FILE": cert, "APIGATE_TLS_KEY_FILE": ""},
            {"APIGATE_TLS_CERT_FILE": str(root / "missing.pem"), "APIGATE_TLS_KEY_FILE": key},
            {"APIGATE_TLS_CERT_FILE": str(root), "APIGATE_TLS_KEY_FILE": key},
            {"APIGATE_TLS_CERT_FILE": str(invalid), "APIGATE_TLS_KEY_FILE": key},
            {"APIGATE_TLS_CERT_FILE": cert, "APIGATE_TLS_KEY_FILE": str(invalid)},
            {"APIGATE_TLS_CERT_FILE": cert, "APIGATE_TLS_KEY_FILE": str(root / "mismatch.key")},
            {"APIGATE_TLS_CERT_FILE": cert, "APIGATE_TLS_KEY_FILE": str(encrypted)}):
        for arguments in ([], ["--check-config"]):
            rejected = run(binary, arguments, environment)
            assert rejected.returncode != 0 and not rejected.stdout
            assert json.loads(rejected.stderr)["category"] == "configuration"
            assert str(root).encode() not in rejected.stderr
            assert b"private-invalid-pem-marker" not in rejected.stderr
            assert b"BEGIN" not in rejected.stderr and b"error:" not in rejected.stderr
        version = run(binary, ["--version"], environment)
        assert version.returncode == 0 and version.stdout.startswith(b"api-gate ") and not version.stderr
    with socket.socket() as occupied:
        occupied.bind(("127.0.0.1", 0))
        occupied.listen()
        checked = run(binary, ["--check-config"], {"APIGATE_TLS_CERT_FILE": cert,
            "APIGATE_TLS_KEY_FILE": key, "APIGATE_LISTEN_PORT": str(occupied.getsockname()[1])})
        assert checked.returncode == 0 and not checked.stderr
        payload = json.loads(checked.stdout)["payload"]
        assert payload["tls_enabled"] is True
        assert str(root).encode() not in checked.stdout and b"BEGIN" not in checked.stdout
    checked = run(binary, ["--check-config"], {})
    assert checked.returncode == 0 and json.loads(checked.stdout)["payload"]["tls_enabled"] is False
    help_result = run(binary, ["--help"], {})
    assert help_result.returncode == 0
    assert b"APIGATE_TLS_CERT_FILE" in help_result.stdout and b"APIGATE_TLS_KEY_FILE" in help_result.stdout


def test_verified_health_and_alpn(binary, root):
    with ExitStack() as resources:
        service, port, client = start(resources, binary, root)
        connection = client()
        original = None
        for path in ("/healthz", "/readyz", "/healthz"):
            response, payload = read_json_response(connection, "GET", path)
            assert response.status == 200 and payload["status"] in ("ok", "ready")
            assert connection.sock.selected_alpn_protocol() == "http/1.1"
            if original is None:
                original = connection.sock
            assert connection.sock is original
        assert scrape(connection)["apigate_active_connections"] == 1
        connection.close()
        for protocols in (None, ("h2", "http/1.1")):
            candidate = client(context(root, protocols))
            assert read_json_response(candidate, "GET", "/healthz")[0].status == 200
            assert candidate.sock.selected_alpn_protocol() == (None if protocols is None else "http/1.1")
            candidate.close()
        tls12 = context(root)
        tls12.maximum_version = ssl.TLSVersion.TLSv1_2
        candidate = client(tls12)
        assert read_json_response(candidate, "GET", "/healthz")[0].status == 200
        assert candidate.sock.version() == "TLSv1.2"
        candidate.close()
        for rejected_context, name in ((context(root, ca="other-ca.pem"), "localhost"),
                                         (context(root), "wrong.invalid"),
                                         (context(root, ("h2",)), "localhost")):
            raw = socket.create_connection(("127.0.0.1", port), timeout=3)
            resources.callback(raw.close)
            try:
                peer = rejected_context.wrap_socket(raw, server_hostname=name)
            except ssl.SSLError:
                pass
            else:
                peer.close()
                raise AssertionError("TLS verification/ALPN rejection did not occur")
        # Sending plaintext to the TLS listener never yields a plaintext HTTP error.
        with socket.create_connection(("127.0.0.1", port), timeout=3) as raw:
            raw.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
            try:
                data = raw.recv(1024)
            except ConnectionError:
                data = b""
            assert not data.startswith(b"HTTP/")
        assert read_json_response(client(), "GET", "/healthz")[0].status == 200
        stop_service(service)
        logs(service, {"certificate location": str(root)})


def test_proxy_and_budget(binary, root):
    with ExitStack() as resources:
        plain = PoolBackend(resources, identity="plain")
        secure = PoolBackend(resources, root, identity="secure")
        routes = [{"prefix": "/plain", "strip_prefix": True, "host": "127.0.0.1", "port": plain.port},
                  {"prefix": "/secure", "strip_prefix": True, "host": "localhost", "port": secure.port,
                   "scheme": "https"}]
        service, _, client = start(resources, binary, root, APIGATE_ROUTES=json.dumps(routes),
            APIGATE_UPSTREAM_CA_FILE=str(root / "ca.pem"), APIGATE_UPSTREAM_KEEP_ALIVE="true",
            APIGATE_MAX_IN_FLIGHT_BYTES="131072", APIGATE_MAX_CONCURRENT_PROXIES="1")
        connection = client()
        marker = "private-tls-request-marker"
        for prefix, backend in (("plain", plain), ("secure", secure)):
            for method, body in (("GET", None), ("POST", b"x" * 65536)):
                connection.request(method, f"/{prefix}/item?raw=%2f&m={marker}", body=body,
                                   headers={"Authorization": marker, "Cookie": marker, "Host": "wrong.invalid"})
                response = connection.getresponse()
                assert response.status == 200 and response.read() == prefix.encode()
                received = backend.state.requests.get(timeout=2)
                assert received["path"] == "/item?raw=%2f&m=" + marker
                assert received["body"] == (body or b"")
                assert received["host"] == f"{'localhost' if prefix == 'secure' else '127.0.0.1'}:{backend.port}"
                values = scrape(connection)
                assert values["apigate_active_proxies"] == values["apigate_in_flight_body_bytes"] == 0
            assert backend.accepts == 1
        assert secure.handshakes == 1
        connection.request("POST", "/plain/large", body=b"x" * 65537)
        response = connection.getresponse()
        assert response.status == 413 and json.loads(response.read())["error"]["code"] == "payload_too_large"
        assert plain.state.requests.empty()
        stop_service(service)
        logs(service, {"private query/header": marker, "certificate location": str(root)})


def test_handshake_deadline_and_capacity(binary, root):
    with ExitStack() as resources:
        service, port, client = start(resources, binary, root, APIGATE_MAX_CONNECTIONS="1")
        expected_responses = 0
        for fragmented in (False, True):
            raw = socket.create_connection(("127.0.0.1", port), timeout=3)
            resources.callback(raw.close)
            service.wait_for_event("http_accept_paused_capacity")
            started = time.monotonic()
            if fragmented:
                raw.sendall(b"\x16\x03\x01\x00\x40\x01\x00\x00\x3c")
            poller = select.poll()
            poller.register(raw, select.POLLIN | select.POLLHUP | select.POLLERR)
            fragments = 0
            deadline = started + 7
            while True:
                assert time.monotonic() < deadline, "TLS handshake exceeded its fixed deadline"
                # Deliberate fragmentation latency injection; socket readiness determines closure.
                ready = poller.poll(700 if fragmented else 6500)
                if ready:
                    assert_peer_closed(raw, 1)
                    break
                raw.sendall(b"\x00")
                fragments += 1
            elapsed = time.monotonic() - started
            assert 4 <= elapsed <= 6.5
            if fragmented:
                assert fragments >= 4
            raw.close()
            failure = service.wait_for_event("http_tls_handshake_failed")
            assert failure["reason"] == "timeout"
            connection = client()
            values = scrape(connection)
            assert sum(value for key, value in values.items() if key.startswith("apigate_http_responses_total")) == expected_responses
            assert values["apigate_active_connections"] == 1
            assert values["apigate_active_proxies"] == values["apigate_in_flight_body_bytes"] == 0
            assert values["apigate_request_processing_seconds_count"] == expected_responses
            assert failures(values) == 0
            assert read_json_response(connection, "GET", "/healthz")[0].status == 200
            expected_responses += 1
            connection.close()
            service.wait_for_event("http_accept_resumed_capacity")
        stop_service(service)
        logs(service)


def pending_shutdown(resources, root, port):
    peer = verified_peer(resources, root, port)
    peer.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    response = http.client.HTTPResponse(peer)
    response.begin()
    assert response.status == 200 and json.loads(response.read())["status"] == "ok"
    assert peer.recv(1) == b""  # actual close_notify, not inferred from a delay
    poller = select.poll()
    poller.register(peer, select.POLLIN | select.POLLHUP | select.POLLERR)
    assert not poller.poll(0), "TCP closed before shutdown cancellation could be tested"
    return peer, poller


def test_shutdown_and_cancellation(binary, root):
    # Cooperative close_notify exchange succeeds immediately with verified TLS.
    with ExitStack() as resources:
        service, port, _ = start(resources, binary, root)
        peer = verified_peer(resources, root, port)
        peer.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        response = http.client.HTTPResponse(peer)
        response.begin()
        assert response.status == 200 and response.read()
        plain = peer.unwrap()
        plain.close()
        stop_service(service)
        logs(service)
    for mode in ("timeout", "drain_deadline", "second_signal"):
        with ExitStack() as resources:
            service, port, client = start(resources, binary, root,
                APIGATE_SHUTDOWN_GRACE_MS="100" if mode == "drain_deadline" else "5000")
            peer, poller = pending_shutdown(resources, root, port)
            started = time.monotonic()
            if mode != "timeout":
                service.terminate()
                service.wait_for_event("shutdown_drain_started")
                if mode == "second_signal":
                    service.terminate(signal.SIGINT)
                    service.wait_for_event("shutdown_force_requested")
            assert poller.poll(3500), "TLS shutdown did not release its TCP socket"
            assert os.read(peer.fileno(), 1) == b""
            if mode == "timeout":
                assert 1 <= time.monotonic() - started <= 3.5
                assert read_json_response(client(), "GET", "/healthz")[0].status == 200
                stop_service(service)
            else:
                assert service.process.wait(timeout=2) == 0
                assert time.monotonic() - started < 1.8
                service.finish_log_reader()
            logs(service)
    with ExitStack() as resources:
        service, port, _ = start(resources, binary, root, APIGATE_MAX_CONNECTIONS="1")
        raw = socket.create_connection(("127.0.0.1", port), timeout=3)
        resources.callback(raw.close)
        service.wait_for_event("http_accept_paused_capacity")
        service.terminate()
        assert_peer_closed(raw, 1)
        assert service.process.wait(timeout=2) == 0
        service.finish_log_reader()
        logs(service)


def test_rst_and_truncated_request(binary, root):
    with ExitStack() as resources:
        backend = PoolBackend(resources)
        service, port, client = start(resources, binary, root, APIGATE_ROUTES=json.dumps([
            {"prefix": "/", "host": "127.0.0.1", "port": backend.port}]))
        raw = socket.create_connection(("127.0.0.1", port), timeout=3)
        raw.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        raw.close()  # RST before handshake; no HTTP response or proxy failure
        connection = client()
        initial = scrape(connection)
        assert all(value == 0 for value in cumulative(initial).values())
        peer = verified_peer(resources, root, port)
        peer.sendall(b"POST /item HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nx")
        deadline = time.monotonic() + 3
        while scrape(connection)["apigate_in_flight_body_bytes"] != 10:
            assert time.monotonic() < deadline, "partial body was not reserved"
        peer.shutdown(socket.SHUT_WR)  # TCP FIN without TLS close_notify, incomplete HTTP body
        service.wait_for_event("http_tls_request_truncated")
        peer.close()
        deadline = time.monotonic() + 3
        while scrape(connection)["apigate_in_flight_body_bytes"] != 0:
            assert time.monotonic() < deadline, "truncated request retained budget"
        assert backend.state.request_count() == 0
        assert cumulative(scrape(connection)) == cumulative(initial)
        reset_peer = verified_peer(resources, root, port)
        reset_peer.sendall(b"POST /item HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nx")
        deadline = time.monotonic() + 3
        while scrape(connection)["apigate_in_flight_body_bytes"] != 10:
            assert time.monotonic() < deadline, "RST request was not reserved"
        reset_peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        reset_peer.close()
        deadline = time.monotonic() + 3
        while scrape(connection)["apigate_in_flight_body_bytes"] != 0:
            assert time.monotonic() < deadline, "RST retained the body reservation"
        assert backend.state.request_count() == 0
        assert read_json_response(connection, "GET", "/healthz")[0].status == 200
        stop_service(service)
        logs(service)


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="apigate-downstream-tls-") as directory:
        root = certificate_files(directory)
        test_configuration(binary, root)
        test_verified_health_and_alpn(binary, root)
        test_proxy_and_budget(binary, root)
        test_handshake_deadline_and_capacity(binary, root)
        test_shutdown_and_cancellation(binary, root)
        test_rst_and_truncated_request(binary, root)
    print("downstream TLS: verified clients, ALPN, proxying, capacity, fixed deadlines and cancellation passed")


if __name__ == "__main__":
    main()
