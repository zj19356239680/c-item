#!/usr/bin/env python3

"""Local TLS contract tests. Disposable test keys never enter production trust."""

import http.client
import http.server
import json
import os
from pathlib import Path
import queue
import secrets
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
from contextlib import ExitStack

from http_proxy_test import ControlledUpstream, ControlledUpstreamHandler, UpstreamState, stop_service
from http_server_test import (
    ServiceProcess, assert_event_absent, assert_normal_log_output, assert_peer_closed,
    assert_sensitive_values_absent, build_test_environment, read_json_response,
)


def certificate_files(directory):
    def openssl(*arguments):
        result = subprocess.run(["openssl", *map(str, arguments)], capture_output=True,
                                timeout=15, check=False)
        assert result.returncode == 0, "local test certificate generation failed"

    # Fresh, throwaway keys; long validity avoids short-lived checked-in fixtures.
    root = Path(directory)
    for name in ("ca", "other-ca"):
        openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                "-noenc", "-days", "36500", "-subj", "/CN=ApiGate disposable test CA",
                "-keyout", root / (name + ".key"), "-out", root / (name + ".pem"),
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    for name, san in (("server", "DNS:localhost,IP:127.0.0.1"),
                      ("mismatch", "DNS:wrong.invalid")):
        openssl("req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                "-noenc", "-subj", "/CN=localhost", "-keyout", root / (name + ".key"),
                "-out", root / (name + ".csr"), "-addext", "subjectAltName=" + san)
        openssl("x509", "-req", "-in", root / (name + ".csr"), "-CA", root / "ca.pem",
                "-CAkey", root / "ca.key", "-set_serial", "1" if name == "server" else "2",
                "-days", "36500", "-copy_extensions", "copy", "-out", root / (name + ".pem"))
    return root


class TlsHandler(ControlledUpstreamHandler):
    def setup(self):
        self.request.do_handshake()
        self.server.negotiated.put((self.request.version(), self.request.selected_alpn_protocol()))
        super().setup()

    def _handle_request(self):
        if self.path == "/budget-full":
            self.server.state.record(self, b"")
            self._send_response(200, b"x" * (1024 * 1024))
            return
        if self.path in ("/truncated", "/unframed"):
            self.server.state.record(self, self.rfile.read(int(self.headers.get("Content-Length", "0"))))
            framing = b"Content-Length: 100\r\n" if self.path == "/truncated" else b""
            self.connection.sendall(b"HTTP/1.1 200 OK\r\n" + framing + b"Connection: close\r\n\r\nshort")
            # Deliberately omit TLS close_notify, both with and without HTTP framing.
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
            return
        super()._handle_request()


class TlsServer(http.server.ThreadingHTTPServer):
    # Non-daemon workers are joined by server_close; every accepted socket is bounded.
    daemon_threads = False

    def get_request(self):
        raw, address = super().get_request()
        raw.settimeout(3)
        try:
            return self.context.wrap_socket(raw, server_side=True, do_handshake_on_connect=False), address
        except BaseException:
            raw.close()
            raise

    def handle_error(self, request, client_address):
        # Trust rejection is an expected TLS alert, not an HTTP handler failure.
        error = sys.exception()
        if not isinstance(error, (ssl.SSLError, ConnectionError)):
            self.unexpected.put(type(error).__name__)


class TlsUpstream(ControlledUpstream):
    def __init__(self, root, name="server"):
        self.state = UpstreamState()
        self.state.identity = "tls-response-marker"
        self.server = TlsServer(("127.0.0.1", 0), TlsHandler)
        self.server.state = self.state
        self.server.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.server.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.server.context.load_cert_chain(root / (name + ".pem"), root / (name + ".key"))
        self.server.context.set_alpn_protocols(["h2", "http/1.1"])
        self.server.sni = queue.Queue()
        self.server.negotiated = queue.Queue()
        self.server.unexpected = queue.Queue()
        self.server.context.set_servername_callback(
            lambda unused_socket, name, unused_context: self.server.sni.put(name))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def close(self):
        super().close()
        assert self.server.unexpected.empty(), "unexpected TLS backend worker error"


class StalledHandshake:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(3)
        self.port = self.listener.getsockname()[1]
        self.started = threading.Event()
        self.closed = threading.Event()
        self.peer = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            self.peer, _ = self.listener.accept()
            self.peer.settimeout(10)
            assert self.peer.recv(4096), "ClientHello was not received"
            self.started.set()
            # Socket readiness/EOF, not a sleep, observes cancellation of the handshake.
            while self.peer.recv(4096):
                pass
            self.closed.set()
        except (OSError, AssertionError):
            pass

    def close(self):
        self.listener.close()
        if self.peer is not None:
            try:
                self.peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.peer.close()
        self.thread.join(timeout=4)
        assert not self.thread.is_alive(), "handshake observer did not finish"


def service_and_connection(resources, binary, routes, ca=None, **extra):
    environment = {"APIGATE_ROUTES": json.dumps(routes), **extra}
    if ca is not None:
        environment["APIGATE_UPSTREAM_CA_FILE"] = str(ca)
    service = ServiceProcess(binary, environment)
    resources.callback(service.process.stdout.close)
    resources.callback(service.cleanup)
    port = int(service.wait_for_event("http_listener_started")["port"])
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    resources.callback(connection.close)
    return service, connection


def logs(service, stages=(), markers=None, forced=False):
    if not forced:
        stop_service(service)
    else:
        assert service.process.wait(timeout=3) == 0
        service.finish_log_reader()
    output = "".join(service.lines)
    records = assert_normal_log_output(output)
    assert all(record["level"] not in ("error", "critical") for record in records)
    failures = [record["payload"] for record in records
                if record["payload"]["event"] == "http_upstream_request_failed"]
    assert [failure["stage"] for failure in failures] == list(stages)
    for event in ("http_response_write_failed", "http_accept_retry_failed",
                  "http_proxy_rejected_capacity", "http_buffer_budget_rejected"):
        assert_event_absent(output, event)
    assert_sensitive_values_absent(output, markers or {})
    return records


def test_forwarding(binary, root):
    with ExitStack() as resources:
        tls = TlsUpstream(root)
        tls.start()
        resources.callback(tls.close)
        plain = ControlledUpstream()
        plain.start()
        plain.state.identity = "plain-response-marker"
        resources.callback(plain.close)
        routes = [{"prefix": "/api", "strip_prefix": True, "upstreams": [
            {"host": "localhost", "port": tls.port, "scheme": "https"},
            {"host": "127.0.0.1", "port": plain.port}],
            "rate_limit": {"requests_per_second": 1, "burst": 100}},
            {"prefix": "/ip", "strip_prefix": True, "host": "127.0.0.1", "port": tls.port,
             "scheme": "https"}]
        service, connection = service_and_connection(resources, binary, routes, root / "ca.pem",
                                                     APIGATE_MAX_CONCURRENT_PROXIES="1",
                                                     APIGATE_MAX_IN_FLIGHT_BYTES="1048576")
        marker = secrets.token_hex(16)
        original_socket = None
        for method, target, backend, expected, host, body in (
            ("POST", "/api/item?x=%2F&x=%2f&empty=&m=" + marker, tls,
             "/item?x=%2F&x=%2f&empty=&m=" + marker, "localhost", marker.encode()),
            ("GET", "/api/healthz?", plain, "/healthz?", "127.0.0.1", None),
            ("GET", "/api/again", tls, "/again", "localhost", None),
            ("POST", "/ip/item?", tls, "/item?", "127.0.0.1", marker.encode()),
        ):
            response, payload = read_json_response(connection, method, target, body,
                                                   {"Host": "wrong.invalid"})
            assert response.status == 201 and payload == {"upstream": backend.state.identity}
            received = backend.state.requests.get(timeout=3)
            assert received["path"] == expected and received["body"] == (body or b"")
            assert received["host"] == f"{host}:{backend.port}"
            if body is not None:
                assert received["content_length"] == str(len(body))
            if original_socket is None:
                original_socket = connection.sock
            assert connection.sock is original_socket and original_socket is not None
        assert [tls.server.sni.get(timeout=3) for _ in range(3)] == ["localhost", "localhost", None]
        for _ in range(3):
            version, protocol = tls.server.negotiated.get(timeout=3)
            assert version in ("TLSv1.2", "TLSv1.3") and protocol == "http/1.1"
        before = (tls.state.request_count(), plain.state.request_count())
        for path in ("/healthz", "/readyz"):
            assert read_json_response(connection, "GET", path)[0].status == 200
        assert before == (tls.state.request_count(), plain.state.request_count())
        # Neither framed nor EOF-delimited incomplete TLS responses may become success.
        for path in ("/ip/truncated", "/ip/unframed"):
            response, payload = read_json_response(connection, "GET", path)
            assert response.status == 502 and payload["error"]["code"] == "bad_gateway"
        response, _ = read_json_response(connection, "POST", "/ip/recovered", b"r" * 64)
        assert response.status == 201
        connection.request("GET", "/ip/budget-full")
        full = connection.getresponse()
        assert full.status == 200 and full.read() == b"x" * (1024 * 1024)
        connection.close()
        logs(service, ("read", "read"), {"query/body": marker, "tls body": tls.state.identity,
                                         "http body": plain.state.identity, "CA path": str(root)})


def test_rejected_identity(binary, root):
    with ExitStack() as resources:
        for cert_name, ca, host in (("server", None, "localhost"),
                                    ("server", root / "other-ca.pem", "localhost"),
                                    ("mismatch", root / "ca.pem", "localhost"),
                                    ("mismatch", root / "ca.pem", "127.0.0.1")):
            backend = TlsUpstream(root, cert_name)
            backend.start()
            resources.callback(backend.close)
            route = {"prefix": "/", "host": host, "port": backend.port, "scheme": "https"}
            with ExitStack() as processes:
                service, connection = service_and_connection(processes, binary, [route], ca)
                response, payload = read_json_response(connection, "GET", "/check",
                                                       headers={"Host": "wrong.invalid"})
                assert response.status == 502 and payload["error"]["code"] == "bad_gateway"
                assert backend.state.request_count() == 0
                assert read_json_response(connection, "GET", "/healthz")[0].status == 200
                connection.close()
                logs(service, ("handshake",), {"CA path": str(root)})


def test_handshake_deadline_and_cancel(binary, root):
    for force in (False, True):
        with ExitStack() as resources:
            stalled = StalledHandshake()
            resources.callback(stalled.close)
            good = TlsUpstream(root)
            good.start()
            resources.callback(good.close)
            routes = [{"prefix": "/stall", "host": "127.0.0.1", "port": stalled.port,
                       "scheme": "https", "timeout_ms": 10000 if force else 150},
                      {"prefix": "/good", "strip_prefix": True, "host": "127.0.0.1", "port": good.port,
                       "scheme": "https"}]
            service, connection = service_and_connection(
                resources, binary, routes, root / "ca.pem", APIGATE_MAX_CONCURRENT_PROXIES="1",
                APIGATE_MAX_IN_FLIGHT_BYTES="128", APIGATE_SHUTDOWN_GRACE_MS="10000")
            connection.request("POST", "/stall", body=b"x" * 64)
            assert stalled.started.wait(timeout=3), "handshake did not start"
            if force:
                service.terminate()
                service.wait_for_event("shutdown_drain_started")
                service.terminate(signal.SIGINT)
                records = logs(service, forced=True)
                for event in ("shutdown_drain_completed", "shutdown_drain_timed_out"):
                    assert_event_absent("".join(service.lines), event)
                assert sum(record["payload"]["event"] == "shutdown_force_requested"
                           for record in records) == 1
                assert_peer_closed(connection.sock, timeout=3)
            else:
                response = connection.getresponse()
                payload = json.loads(response.read())
                assert response.status == 504 and payload["error"]["code"] == "gateway_timeout"
                assert read_json_response(connection, "POST", "/good", b"x" * 64)[0].status == 201
                connection.request("POST", "/good/no-content", body=b"x" * 128)
                recovered = connection.getresponse()
                assert recovered.status == 204 and recovered.read() == b""
                connection.close()
                logs(service, ("timeout",))
            assert stalled.closed.wait(timeout=3), "upstream handshake socket was not cancelled"


def test_trust_config_cli(binary, root):
    for ca, valid in ((root / "ca.pem", True), (root / "missing-private-ca.pem", False),
                      (root / "server.key", False), ("", False)):
        for arguments in ((["--check-config"],) if valid else (["--check-config"], [])):
            result = subprocess.run([binary, *arguments], capture_output=True, text=True, timeout=5,
                                    env=build_test_environment({"APIGATE_UPSTREAM_CA_FILE": str(ca)}),
                                    check=False)
            assert (result.returncode == 0) == valid
            assert str(root) not in result.stdout + result.stderr
            if valid:
                assert json.loads(result.stdout)["payload"]["proxy_enabled"] is False
                assert not result.stderr
            else:
                assert not result.stdout
                assert json.loads(result.stderr)["category"] == "configuration"


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: https_proxy_test.py <api-gate-binary>")
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="apigate-tls-test-") as directory:
        root = certificate_files(directory)
        test_trust_config_cli(binary, root)
        test_forwarding(binary, root)
        test_rejected_identity(binary, root)
        test_handshake_deadline_and_cancel(binary, root)
    print("HTTPS: trust, DNS/IP identity, SNI/ALPN, mixed round robin, framing, deadline and force cancel passed")


if __name__ == "__main__":
    main()
