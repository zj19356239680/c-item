#!/usr/bin/env python3
"""Bounded streaming, real early body delivery, framing and cancellation."""
from contextlib import ExitStack
import http.client
import json
from pathlib import Path
import signal
import socket
import sys
import tempfile
import threading
import time

from downstream_tls_test import context
from http_server_test import ServiceProcess
from http_proxy_test import stop_service
from https_proxy_test import certificate_files
from metrics_test import scrape, failures, response_count, cumulative
from upstream_pool_test import PoolBackend, PoolHandler

LIMIT = 1024 * 1024
FIRST = b"first"
LAST = b"-last"


class StreamHandler(PoolHandler):
    def _handle_request(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.state.record(self, body)
        path = self.path.split("?", 1)[0]
        if path == "/oversize":
            self.connection.sendall(f"HTTP/1.1 200 OK\r\nContent-Length: {LIMIT + 1}\r\n\r\n".encode())
            self.close_connection = True
            return
        if path == "/none":
            self.connection.sendall(b"HTTP/1.1 204 No Content\r\n\r\n")
            return
        chunked = path in ("/chunked", "/late-large", "/chunk-truncated")
        eof = path == "/eof"
        header = b"HTTP/1.1 200 OK\r\nX-End-To-End: kept\r\nX-Remove: secret\r\nConnection: X-Remove\r\n"
        if chunked:
            header += b"Transfer-Encoding: chunked\r\n"
        elif eof:
            header += b"Connection: close\r\n"
        else:
            header += b"Content-Length: 10\r\n"
        prefix = b"5\r\nfirst\r\n" if chunked else FIRST
        self.connection.sendall(header + b"\r\n" + prefix)
        self.server.blocked.set()
        if path in ("/pause", "/truncated", "/timeout", "/chunk-truncated"):
            if not self.server.release.wait(timeout=6):
                return
        if path in ("/truncated", "/timeout", "/chunk-truncated"):
            self.close_connection = True
            return
        if path == "/late-large":
            # Parsed bytes, not encoded chunk metadata, exceed the total bound.
            self.connection.sendall(f"{LIMIT:x}\r\n".encode() + b"x" * LIMIT + b"\r\n0\r\n\r\n")
            return
        suffix = b"5\r\n-last\r\n0\r\nX-Trailer: not-forwarded\r\n\r\n" if chunked else LAST
        self.connection.sendall(suffix)
        if eof:
            self.close_connection = True


def backend(resources, root=None):
    result = PoolBackend(resources, root)
    result.RequestHandlerClass = StreamHandler
    return result


def start(resources, binary, upstream, root, streaming=True, down_tls=False, up_tls=False,
          timeout=1800, **extra):
    route = {"prefix": "/s", "strip_prefix": True, "host": "127.0.0.1", "port": upstream.port,
             "scheme": "https" if up_tls else "http", "timeout_ms": timeout}
    if streaming is not None:
        route["stream_response"] = streaming
    environment = {"APIGATE_ROUTES": json.dumps([route]), "APIGATE_METRICS_ENABLED": "true",
                   "APIGATE_UPSTREAM_KEEP_ALIVE": "true", **extra}
    if up_tls:
        environment["APIGATE_UPSTREAM_CA_FILE"] = str(root / "ca.pem")
    if down_tls:
        environment.update(APIGATE_TLS_CERT_FILE=str(root / "server.pem"),
                           APIGATE_TLS_KEY_FILE=str(root / "server.key"))
    service = ServiceProcess(binary, environment)
    resources.callback(service.process.stdout.close)
    resources.callback(service.cleanup)
    port = int(service.wait_for_event("http_listener_started")["port"])

    def client():
        connection = (http.client.HTTPSConnection("127.0.0.1", port, context=context(root), timeout=4)
                      if down_tls else http.client.HTTPConnection("127.0.0.1", port, timeout=4))
        resources.callback(connection.close)
        return connection
    return service, port, client


def recovered(client):
    # Scrapes serialize after completion, without adding cumulative observations.
    connection = client()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        values = scrape(connection)
        if values["apigate_active_proxies"] == 0 and values["apigate_in_flight_body_bytes"] == 0:
            return values
    raise AssertionError("stream permit/body budget was not recovered")


def test_early_and_default(binary, root):
    for enabled in (None, True):
        for down_tls, up_tls in ((False, False), (True, False), (False, True), (True, True)):
            with ExitStack() as resources:
                upstream = backend(resources, root if up_tls else None)
                service, _, client = start(resources, binary, upstream, root, enabled, down_tls, up_tls)
                connection = client()
                connection.request("POST", "/s/pause?kept=yes", body=b"bounded-post")
                assert upstream.blocked.wait(3)
                if enabled:
                    response = connection.getresponse()
                    assert response.status == 200
                    assert response.getheader("Content-Length") == "10"
                    assert response.getheader("X-End-To-End") == "kept"
                    assert response.getheader("X-Remove") is None
                    assert response.read(5) == FIRST  # Body arrived before release, not just headers.
                    assert not upstream.release.is_set()
                    values = scrape(client())
                    assert values["apigate_active_proxies"] == 1
                    assert 0 < values["apigate_in_flight_body_bytes"] <= 16384 + len(b"bounded-post")
                else:
                    # Backend already sent its header/prefix. Buffered mode exposes neither.
                    response_ready = threading.Event()
                    buffered_result = []
                    def buffered_reader():
                        try:
                            candidate = connection.getresponse()
                            buffered_result.append((candidate, candidate.read()))
                        except BaseException as error:
                            buffered_result.append(error)
                        finally:
                            response_ready.set()
                    reader = threading.Thread(target=buffered_reader)
                    reader.start()
                    resources.callback(reader.join, 5)
                    assert not response_ready.wait(0.1)
                upstream.release.set()
                if not enabled:
                    assert response_ready.wait(4)
                    reader.join(timeout=1)
                    assert not reader.is_alive()
                    response, payload = buffered_result[0]
                    assert payload == FIRST + LAST
                else:
                    assert response.read() == LAST
                # Same downstream keep-alive and cross-client upstream reuse remain safe.
                connection.request("GET", "/s/complete")
                assert connection.getresponse().read() == FIRST + LAST
                other = client()
                other.request("GET", "/s/complete")
                assert other.getresponse().read() == FIRST + LAST
                assert upstream.accepts == 1
                assert upstream.handshakes == (1 if up_tls else 0)
                values = recovered(client)
                assert response_count(values, 200) == 3 and failures(values) == 0
                assert cumulative(scrape(client())) == cumulative(values)
                stop_service(service)
                output = "".join(service.lines)
                assert "kept=yes" not in output and "bounded-post" not in output
    print("early body/default buffer: all four HTTP/HTTPS combinations; accepts=1; TLS handshakes=1", flush=True)


def test_framing_and_small_budget(binary, root):
    with ExitStack() as resources:
        upstream = backend(resources)
        service, port, client = start(resources, binary, upstream, root,
                                      APIGATE_MAX_IN_FLIGHT_BYTES="7")
        connection = client()
        for path in ("/complete", "/chunked", "/eof", "/none"):
            connection.request("GET", "/s" + path)
            response = connection.getresponse()
            assert response.status == (204 if path == "/none" else 200)
            assert response.read() == (b"" if path == "/none" else FIRST + LAST)
            assert response.getheader("X-Trailer") is None
            if path in ("/chunked", "/eof"):
                assert response.getheader("Transfer-Encoding") == "chunked"
                assert response.getheader("Content-Length") is None
        # HTTP/1.0 unknown size uses connection close, never chunked encoding.
        with socket.create_connection(("127.0.0.1", port), timeout=3) as peer:
            peer.sendall(b"GET /s/chunked HTTP/1.0\r\nHost: localhost\r\n\r\n")
            response = http.client.HTTPResponse(peer)
            response.begin()
            assert response.version == 10 and response.getheader("Transfer-Encoding") is None
            assert response.read() == FIRST + LAST
        values = recovered(client)
        assert values["apigate_in_flight_body_bytes"] == 0
        stop_service(service)
    print("Content-Length/chunked/EOF/no-body/HTTP1.0: correct with 7-byte budget", flush=True)


def raw_failure(client, path):
    connection = client()
    connection.request("GET", "/s" + path)
    response = connection.getresponse()
    assert response.status == 200
    assert response.read(5) == FIRST
    try:
        response.read()
    except http.client.IncompleteRead as error:
        assert b"gateway" not in error.partial and b"error" not in error.partial
    else:
        raise AssertionError("late failure was presented as a completed response")
    connection.close()


def test_failures(binary, root):
    for path, stage in (("/truncated", "read"), ("/chunk-truncated", "read"),
                        ("/late-large", "response_too_large"), ("/timeout", "timeout")):
        with ExitStack() as resources:
            upstream = backend(resources)
            if path != "/timeout":
                upstream.release.set()
            service, _, client = start(resources, binary, upstream, root, timeout=350)
            raw_failure(client, path)
            values = recovered(client)
            assert response_count(values, 200) == 1 and response_count(values, 502) == 0
            assert response_count(values, 504) == 0 and failures(values) == 1
            assert values[f'apigate_upstream_failures_total{{stage="{stage}"}}'] == 1
            # The failed connection was discarded, not returned or retried.
            connection = client()
            connection.request("GET", "/s/complete")
            assert connection.getresponse().read() == FIRST + LAST
            assert upstream.accepts == 2
            stop_service(service)
    # Both pre-header error mappings remain safe, without emitting upstream status.
    with ExitStack() as resources:
        upstream = backend(resources)
        service, _, client = start(resources, binary, upstream, root, APIGATE_MAX_IN_FLIGHT_BYTES="1")
        for method, path, body, expected in (("GET", "/oversize", None, 502),
                                             ("POST", "/complete", b"x", 503)):
            connection = client()
            connection.request(method, "/s" + path, body)
            response = connection.getresponse()
            assert response.status == expected and json.loads(response.read())["error"]
        recovered(client)
        stop_service(service)
    print("pre-header size/budget errors and late truncation/limit/timeout: mapped or closed correctly", flush=True)


def test_drain_and_disconnect(binary, root):
    for down_tls in (False, True):
        for action in ("disconnect", "drain", "deadline", "second"):
            with ExitStack() as resources:
                upstream = backend(resources)
                service, _, client = start(resources, binary, upstream, root, down_tls=down_tls,
                    APIGATE_SHUTDOWN_GRACE_MS="250" if action == "deadline" else "2000")
                connection = client()
                connection.request("GET", "/s/pause")
                response = connection.getresponse()
                assert response.read(5) == FIRST
                if action == "disconnect":
                    response.close()
                    connection.close()
                    upstream.release.set()
                    assert failures(recovered(client)) == 0
                    candidate = client()
                    candidate.request("GET", "/s/complete")
                    assert candidate.getresponse().read() == FIRST + LAST
                    recovered(client)
                    stop_service(service)
                else:
                    service.process.send_signal(signal.SIGTERM)
                    service.wait_for_event("shutdown_drain_started")
                    if action == "drain":
                        upstream.release.set()
                        assert response.read() == LAST
                    elif action == "second":
                        service.process.send_signal(signal.SIGTERM)
                    assert service.process.wait(timeout=4) == 0
                    service.finish_log_reader()
                    service.wait_for_event("service_stopped")
                    assert not any("http_upstream_request_failed" in line for line in service.lines)
                    if action != "drain":
                        try:
                            response.read()
                        except (http.client.IncompleteRead, ConnectionError, OSError):
                            pass
                        else:
                            raise AssertionError("cancelled stream completed normally")
    print("HTTP/HTTPS disconnect/drain/deadline/second signal: terminated with bounded cleanup", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="apigate-stream-") as directory:
        root = Path(directory)
        certificate_files(root)
        test_early_and_default(binary, root)
        test_framing_and_small_budget(binary, root)
        test_failures(binary, root)
        test_drain_and_disconnect(binary, root)


if __name__ == "__main__":
    main()
