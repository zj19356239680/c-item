#!/usr/bin/env python3

"""Run repeatable local buffered-body capacity baseline scenarios."""

import argparse
import collections
import dataclasses
import hashlib
import http.client
import http.server
import json
import os
import pathlib
import queue
import signal
import socket
import subprocess
import sys
import threading
import time


MAX_REQUEST_BODY = 64 * 1024
EXPECTED_BUDGET_ERROR = {
    "error": {
        "code": "gateway_overloaded",
        "message": "buffer capacity is exhausted",
    }
}
APIGATE_ENVIRONMENT_VARIABLES = (
    "APIGATE_SERVICE_NAME",
    "APIGATE_ENVIRONMENT",
    "APIGATE_LOG_LEVEL",
    "APIGATE_LISTEN_ADDRESS",
    "APIGATE_LISTEN_PORT",
    "APIGATE_MAX_CONNECTIONS",
    "APIGATE_MAX_CONCURRENT_PROXIES",
    "APIGATE_MAX_IN_FLIGHT_BYTES",
    "APIGATE_SHUTDOWN_GRACE_MS",
    "APIGATE_UPSTREAM_HOST",
    "APIGATE_UPSTREAM_PORT",
    "APIGATE_UPSTREAM_TIMEOUT_MS",
    "APIGATE_UPSTREAM_CA_FILE",
    "APIGATE_ROUTES",
)


class BaselineError(RuntimeError):
    """A correctness constraint or bounded operation failed."""


@dataclasses.dataclass(frozen=True)
class HttpResult:
    status: int
    body: bytes


@dataclasses.dataclass
class ScenarioMetrics:
    request_count: int = 0
    success_count: int = 0
    body_budget_503_count: int = 0
    other_error_count: int = 0
    saturation_probe_count: int = 0
    duration_seconds: float = 0.0

    def as_json(self, rounds, contenders):
        return {
            "rounds": rounds,
            "contender_count": rounds * contenders,
            "request_count": self.request_count,
            "success_count": self.success_count,
            "body_budget_503_count": self.body_budget_503_count,
            "other_error_count": self.other_error_count,
            "saturation_probe_count": self.saturation_probe_count,
            "duration_ms": round(self.duration_seconds * 1000.0, 3),
        }


class BaselineUpstreamState:
    def __init__(self, timeout_seconds):
        self.timeout_seconds = timeout_seconds
        self._counts = collections.Counter()
        self._lock = threading.Lock()
        self._slow_header_sent = threading.Event()
        self._slow_release = threading.Event()
        self._slow_body = b""
        self._failures = []

    def record_request(self, route):
        with self._lock:
            self._counts[route] += 1

    def route_count(self, route):
        with self._lock:
            return self._counts[route]

    def prepare_slow_response(self, body):
        with self._lock:
            self._slow_header_sent = threading.Event()
            self._slow_release = threading.Event()
            self._slow_body = body
            return self._slow_header_sent, self._slow_release

    def slow_response_state(self):
        with self._lock:
            return self._slow_header_sent, self._slow_release, self._slow_body

    def record_failure(self, category):
        with self._lock:
            self._failures.append(category)

    def failures(self):
        with self._lock:
            return list(self._failures)

    def release_waiters(self):
        with self._lock:
            self._slow_release.set()


class BaselineUpstreamHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, unused_format, *unused_args):
        pass

    def do_GET(self):
        self._handle_request()

    def do_POST(self):
        self._handle_request()

    def _handle_request(self):
        state = self.server.state
        route = self.path.split("?", 1)[0]
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            if content_length:
                self.rfile.read(content_length)
            state.record_request(route)

            if route in ("/slow-upload", "/request-contender"):
                self._send_no_content()
                return
            if route == "/slow-response":
                header_sent, release, body = state.slow_response_state()
                self._send_headers(200, len(body))
                header_sent.set()
                if not release.wait(timeout=state.timeout_seconds):
                    state.record_failure("slow_response_release_timeout")
                    return
                self._write_body(body)
                return
            if route == "/response-contender":
                _, _, body = state.slow_response_state()
                self._send_headers(200, len(body))
                self._write_body(body)
                return
            self._send_headers(404, 0)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            # ApiGate intentionally closes rejected upstream exchanges.
            return
        except Exception:
            state.record_failure("unexpected_upstream_handler_error")

    def _send_headers(self, status, content_length):
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(content_length))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.flush()

    def _send_no_content(self):
        self._send_headers(204, 0)

    def _write_body(self, body):
        if body:
            self.wfile.write(body)
            self.wfile.flush()


class ThreadingBaselineServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 512

    def __init__(self, *args):
        self.active_condition = threading.Condition()
        self.active_sockets = set()
        self.handler_threads = []
        super().__init__(*args)

    def process_request(self, request, client_address):
        request.settimeout(self.state.timeout_seconds)
        with self.active_condition:
            self.active_sockets.add(request)
        try:
            super().process_request(request, client_address)
        except BaseException:
            with self.active_condition:
                self.active_sockets.discard(request)
            self.shutdown_request(request)
            raise

    def process_request_thread(self, request, client_address):
        with self.active_condition:
            self.handler_threads.append(threading.current_thread())
        try:
            super().process_request_thread(request, client_address)
        finally:
            with self.active_condition:
                self.active_sockets.discard(request)
                self.active_condition.notify_all()

    def finish_handlers(self, timeout):
        deadline = time.monotonic() + timeout
        with self.active_condition:
            for client in self.active_sockets:
                try:
                    client.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass  # A handler may already have closed its socket.
            while self.active_sockets:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise BaselineError("upstream handlers did not stop")
                self.active_condition.wait(timeout=remaining)
        for thread in self.handler_threads:
            thread.join(timeout=max(0.0, deadline - time.monotonic()))
            if thread.is_alive():
                raise BaselineError("an upstream handler thread did not stop")

    def handle_error(self, request, client_address):
        self.state.record_failure("upstream_server_error")


class BaselineUpstream:
    def __init__(self, timeout_seconds):
        self.state = BaselineUpstreamState(timeout_seconds)
        self.server = ThreadingBaselineServer(
            ("127.0.0.1", 0), BaselineUpstreamHandler
        )
        self.server.state = self.state
        self.thread = threading.Thread(
            target=self.server.serve_forever,
            kwargs={"poll_interval": 0.05},
            name="capacity-upstream",
            daemon=True,
        )

    @property
    def port(self):
        return int(self.server.server_address[1])

    def start(self):
        self.thread.start()

    def close(self, timeout_seconds):
        self.state.release_waiters()
        shutdown = threading.Thread(
            target=self.server.shutdown,
            name="capacity-upstream-shutdown",
            daemon=True,
        )
        shutdown.start()
        shutdown.join(timeout=timeout_seconds)
        if shutdown.is_alive():
            raise BaselineError("upstream shutdown exceeded its timeout")
        self.server.server_close()
        self.server.finish_handlers(timeout_seconds)
        self.thread.join(timeout=timeout_seconds)
        if self.thread.is_alive():
            raise BaselineError("upstream thread did not stop")


class ApiGateProcess:
    def __init__(self, binary, upstream_port, reservation_bytes, contenders, timeout):
        environment = os.environ.copy()
        for name in APIGATE_ENVIRONMENT_VARIABLES:
            environment.pop(name, None)
        upstream_timeout_ms = min(60000, max(5000, int(timeout * 4000)))
        environment.update(
            {
                "APIGATE_LISTEN_ADDRESS": "127.0.0.1",
                "APIGATE_LISTEN_PORT": "0",
                "APIGATE_LOG_LEVEL": "info",
                "APIGATE_MAX_CONNECTIONS": str(contenders + 16),
                "APIGATE_MAX_CONCURRENT_PROXIES": str(contenders + 4),
                "APIGATE_MAX_IN_FLIGHT_BYTES": str(reservation_bytes),
                "APIGATE_SHUTDOWN_GRACE_MS": "5000",
                "APIGATE_UPSTREAM_HOST": "127.0.0.1",
                "APIGATE_UPSTREAM_PORT": str(upstream_port),
                "APIGATE_UPSTREAM_TIMEOUT_MS": str(upstream_timeout_ms),
            }
        )
        self.process = subprocess.Popen(
            [str(binary)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            env=environment,
        )
        self._condition = threading.Condition()
        self._records = []
        self._raw_lines = []
        self._invalid_logs = []
        self._stderr_lines = []
        self._readers = [
            threading.Thread(
                target=self._read_stdout, name="capacity-log-stdout", daemon=True
            ),
            threading.Thread(
                target=self._read_stderr, name="capacity-log-stderr", daemon=True
            ),
        ]
        for reader in self._readers:
            reader.start()

    @property
    def pid(self):
        return self.process.pid

    def _read_stdout(self):
        assert self.process.stdout is not None
        for line_number, line in enumerate(self.process.stdout, start=1):
            with self._condition:
                self._raw_lines.append(line)
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    self._invalid_logs.append(self._safe_line_summary(line_number, line))
                else:
                    payload = record.get("payload") if isinstance(record, dict) else None
                    if not isinstance(payload, dict):
                        self._invalid_logs.append(
                            self._safe_line_summary(line_number, line)
                        )
                    else:
                        self._records.append(record)
                self._condition.notify_all()

    def _read_stderr(self):
        assert self.process.stderr is not None
        for line in self.process.stderr:
            with self._condition:
                self._stderr_lines.append(line)
                self._raw_lines.append(line)
                self._condition.notify_all()

    @staticmethod
    def _safe_line_summary(line_number, line):
        encoded = line.encode("utf-8", errors="replace")
        return {
            "line": line_number,
            "length": len(encoded),
            "sha256": hashlib.sha256(encoded).hexdigest()[:16],
        }

    def mark(self):
        with self._condition:
            return len(self._records)

    def wait_for_event(self, event_name, timeout, since=0, direction=None):
        deadline = time.monotonic() + timeout
        with self._condition:
            while True:
                for record in self._records[since:]:
                    payload = record["payload"]
                    if payload.get("event") != event_name:
                        continue
                    if direction is not None and payload.get("direction") != direction:
                        continue
                    return payload
                if self.process.poll() is not None:
                    raise BaselineError("ApiGate exited before the expected log event")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise BaselineError("expected ApiGate log event was not observed")
                self._condition.wait(timeout=remaining)

    def stop(self, timeout):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        try:
            return_code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise BaselineError("ApiGate graceful shutdown exceeded its timeout") from error
        self._join_readers(timeout)
        if return_code != 0:
            raise BaselineError("ApiGate returned a nonzero status")

    def cleanup(self, timeout):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=min(timeout, 2.0))
            except subprocess.TimeoutExpired:
                self.process.send_signal(signal.SIGTERM)
                try:
                    self.process.wait(timeout=min(timeout, 2.0))
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=timeout)
        self._join_readers(timeout)
        self.process.stdout.close()
        self.process.stderr.close()

    def _join_readers(self, timeout, fail_if_alive=True):
        deadline = time.monotonic() + timeout
        for reader in self._readers:
            reader.join(timeout=max(0.0, deadline - time.monotonic()))
        if fail_if_alive and any(reader.is_alive() for reader in self._readers):
            raise BaselineError("ApiGate log reader did not stop")

    def snapshot_logs(self):
        with self._condition:
            return (
                list(self._records),
                "".join(self._raw_lines),
                list(self._invalid_logs),
                len(self._stderr_lines),
            )


class RssSampler:
    def __init__(self, pid, interval_seconds=0.02):
        self.pid = pid
        self.interval_seconds = interval_seconds
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._peak_kib = 0
        self._failed = False
        self._thread = threading.Thread(
            target=self._run, name="capacity-rss-sampler", daemon=True
        )

    def start(self, initial_kib):
        with self._lock:
            self._peak_kib = initial_kib
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            try:
                rss_kib = read_rss_kib(self.pid, missing_ok=True)
            except (OSError, BaselineError, ValueError):
                self._failed = True
                return
            if rss_kib is None:
                return
            with self._lock:
                self._peak_kib = max(self._peak_kib, rss_kib)
            # This short periodic wait is sampling cadence, never flow synchronization.
            self._stop.wait(self.interval_seconds)

    def stop(self, timeout):
        self._stop.set()
        self._thread.join(timeout=timeout)
        if self._thread.is_alive():
            raise BaselineError("RSS sampler did not stop")
        if self._failed:
            raise BaselineError("RSS sampling failed")
        with self._lock:
            return self._peak_kib


def read_rss_kib(pid, missing_ok=False):
    try:
        status = pathlib.Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    except FileNotFoundError:
        if missing_ok:
            return None
        raise BaselineError("ApiGate RSS status was unavailable") from None
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            fields = line.split()
            if len(fields) == 3 and fields[2] == "kB":
                return int(fields[1])
            break
    raise BaselineError("ApiGate RSS status did not contain a valid VmRSS")


def make_body(marker, size, fill):
    if size < 1:
        raise BaselineError("body size must be positive")
    return marker + fill * (size - 1)


class ClientSockets:
    """Allow the owner to interrupt reads before joining failed client groups."""

    def __init__(self):
        self._lock = threading.Lock()
        self._sockets = set()
        self._cancelled = False

    def add(self, client):
        with self._lock:
            if self._cancelled:
                client.close()
                raise BaselineError("client group was cancelled")
            self._sockets.add(client)

    def remove(self, client):
        with self._lock:
            self._sockets.discard(client)

    def cancel(self):
        with self._lock:
            self._cancelled = True
            for client in self._sockets:
                try:
                    client.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass  # A concurrent completed client may already be closed.


def send_http_request(port, method, path, timeout, body=None, sockets=None):
    client = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        if sockets is not None:
            sockets.add(client)
        payload = body or b""
        header = (
            f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n"
            f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n"
        ).encode("ascii")
        client.sendall(header + payload)
        with http.client.HTTPResponse(client) as response:
            response.begin()
            return HttpResult(response.status, response.read())
    except (ConnectionError, http.client.HTTPException, OSError, socket.timeout) as error:
        raise BaselineError("HTTP client operation failed") from error
    finally:
        if sockets is not None:
            sockets.remove(client)
        client.close()


def is_budget_rejection(result):
    if result.status != 503:
        return False
    try:
        payload = json.loads(result.body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return False
    return payload == EXPECTED_BUDGET_ERROR


def require_budget_rejection(result):
    if not is_budget_rejection(result):
        raise BaselineError(
            f"expected buffer-budget 503, received status {result.status} "
            f"and {len(result.body)} body bytes"
        )


def require_response(result, status, body):
    if result.status != status or result.body != body:
        raise BaselineError(
            f"expected status {status} and {len(body)} body bytes, received "
            f"status {result.status} and {len(result.body)} body bytes"
        )


def run_parallel_requests(count, timeout, request_factory):
    barrier = threading.Barrier(count + 1)
    results = [None] * count
    sockets = ClientSockets()

    def worker(index):
        try:
            barrier.wait(timeout=timeout)
            results[index] = request_factory(sockets=sockets)
        except Exception:
            results[index] = BaselineError("concurrent client operation failed")

    threads = [
        threading.Thread(
            target=worker,
            args=(index,),
            name=f"capacity-contender-{index}",
            daemon=True,
        )
        for index in range(count)
    ]
    try:
        for thread in threads:
            thread.start()
        barrier.wait(timeout=timeout)
    except BaseException:
        barrier.abort()
        sockets.cancel()
        raise
    finally:
        # Release a partially started group too; each HTTP operation has a socket timeout.
        deadline = time.monotonic() + timeout * 3
        for thread in threads:
            if thread.ident is not None:
                thread.join(timeout=max(0.0, deadline - time.monotonic()))
        sockets.cancel()
        deadline = time.monotonic() + timeout
        for thread in threads:
            if thread.ident is not None and thread.is_alive():
                thread.join(timeout=max(0.0, deadline - time.monotonic()))
    if any(thread.is_alive() for thread in threads):
        raise BaselineError("concurrent clients did not finish before the timeout")
    if any(isinstance(result, Exception) for result in results):
        raise BaselineError("one or more concurrent clients failed")
    return results


def wait_until_saturated(
    request_factory, success_validator, service, direction, timeout
):
    deadline = time.monotonic() + timeout
    attempts = 0
    successes = 0
    while time.monotonic() < deadline:
        mark = service.mark()
        result = request_factory(max(0.001, deadline - time.monotonic()))
        attempts += 1
        if is_budget_rejection(result):
            rejection = service.wait_for_event(
                "http_buffer_budget_rejected",
                timeout=max(0.01, deadline - time.monotonic()),
                since=mark,
                direction=direction,
            )
            if rejection.get("active_bytes") != rejection.get("max_bytes"):
                raise BaselineError("rejection did not prove the budget was fully held")
            return attempts, successes
        success_validator(result)
        successes += 1
    raise BaselineError("buffer budget did not become saturated before the timeout")


def run_slow_request_scenario(
    port, reservation_bytes, contenders, timeout, upstream, service, metrics
):
    started = time.monotonic()
    request_body = make_body(b"~", reservation_bytes, b"r")
    prefix_size = 0 if reservation_bytes == 1 else min(1024, reservation_bytes - 1)
    holder = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    holder.settimeout(timeout)
    initial_upstream_count = upstream.state.route_count("/slow-upload")
    try:
        header = (
            "POST /slow-upload HTTP/1.1\r\n"
            "Host: localhost\r\n"
            f"Content-Length: {reservation_bytes}\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii")
        holder.sendall(header + request_body[:prefix_size])

        def contender_request(request_timeout=timeout, sockets=None):
            return send_http_request(
                port, "POST", "/request-contender", request_timeout,
                body=b"~", sockets=sockets
            )

        attempts, probe_successes = wait_until_saturated(
            contender_request,
            lambda result: require_response(result, 204, b""),
            service,
            "request",
            timeout,
        )
        before_contenders = upstream.state.route_count("/request-contender")
        results = run_parallel_requests(contenders, timeout, contender_request)
        for result in results:
            require_budget_rejection(result)
        after_contenders = upstream.state.route_count("/request-contender")
        if after_contenders != before_contenders:
            raise BaselineError("a rejected request contender reached the upstream")

        holder.sendall(request_body[prefix_size:])
        holder_response = http.client.HTTPResponse(holder)
        holder_response.begin()
        require_response(
            HttpResult(holder_response.status, holder_response.read()), 204, b""
        )
        if upstream.state.route_count("/slow-upload") != initial_upstream_count + 1:
            raise BaselineError("the slow upload did not reach the upstream exactly once")

        recovered = send_http_request(
            port, "POST", "/request-contender", timeout, body=request_body
        )
        require_response(recovered, 204, b"")

        metrics.request_count += 1 + attempts + contenders + 1
        metrics.success_count += 2 + probe_successes
        metrics.body_budget_503_count += 1 + contenders
        metrics.saturation_probe_count += attempts
    except Exception:
        metrics.other_error_count += 1
        raise
    finally:
        holder.close()
        metrics.duration_seconds += time.monotonic() - started


def run_slow_response_scenario(
    port, reservation_bytes, contenders, timeout, upstream, service, metrics
):
    started = time.monotonic()
    response_body = make_body(b"^", reservation_bytes, b"s")
    header_sent, release_body = upstream.state.prepare_slow_response(response_body)
    holder_result = queue.Queue(maxsize=1)
    holder_sockets = ClientSockets()

    def hold_response_budget():
        try:
            holder_result.put(
                send_http_request(
                    port, "GET", "/slow-response", timeout, sockets=holder_sockets
                )
            )
        except Exception:
            holder_result.put(BaselineError("slow response client failed"))

    holder_thread = threading.Thread(
        target=hold_response_budget,
        name="capacity-slow-response-client",
        daemon=True,
    )
    holder_thread.start()
    try:
        if not header_sent.wait(timeout=timeout):
            raise BaselineError("the slow upstream response header was not observed")

        def contender_request(request_timeout=timeout, sockets=None):
            return send_http_request(
                port, "GET", "/response-contender", request_timeout, sockets=sockets
            )

        attempts, probe_successes = wait_until_saturated(
            contender_request,
            lambda result: require_response(result, 200, response_body),
            service,
            "response",
            timeout,
        )
        results = run_parallel_requests(contenders, timeout, contender_request)
        for result in results:
            require_budget_rejection(result)

        release_body.set()
        holder_thread.join(timeout=timeout)
        if holder_thread.is_alive():
            raise BaselineError("the slow response client did not finish")
        held_result = holder_result.get(timeout=timeout)
        if isinstance(held_result, Exception):
            raise BaselineError("the slow response client failed")
        require_response(held_result, 200, response_body)

        recovered = contender_request()
        require_response(recovered, 200, response_body)

        metrics.request_count += 1 + attempts + contenders + 1
        metrics.success_count += 2 + probe_successes
        metrics.body_budget_503_count += 1 + contenders
        metrics.saturation_probe_count += attempts
    except Exception:
        metrics.other_error_count += 1
        raise
    finally:
        release_body.set()
        holder_sockets.cancel()
        holder_thread.join(timeout=timeout)
        if holder_thread.is_alive():
            raise BaselineError("slow response client remained active during cleanup")
        metrics.duration_seconds += time.monotonic() - started


def validate_logs(service, request_metrics, response_metrics):
    records, raw_logs, invalid_logs, stderr_count = service.snapshot_logs()
    if invalid_logs:
        raise BaselineError("ApiGate emitted invalid structured logs")
    if stderr_count:
        raise BaselineError("ApiGate emitted fallback diagnostics")
    if "~" in raw_logs or "^" in raw_logs:
        raise BaselineError("a request or response body marker appeared in ApiGate logs")

    payloads = [record["payload"] for record in records]
    request_rejections = sum(
        payload.get("event") == "http_buffer_budget_rejected"
        and payload.get("direction") == "request"
        for payload in payloads
    )
    response_rejections = sum(
        payload.get("event") == "http_buffer_budget_rejected"
        and payload.get("direction") == "response"
        for payload in payloads
    )
    proxy_rejections = sum(
        payload.get("event") == "http_proxy_rejected_capacity"
        for payload in payloads
    )
    critical_events = sum(
        record.get("level") == "critical" for record in records
    )
    if request_rejections != request_metrics.body_budget_503_count:
        raise BaselineError("request-direction rejection log count was inconsistent")
    if response_rejections != response_metrics.body_budget_503_count:
        raise BaselineError("response-direction rejection log count was inconsistent")
    if proxy_rejections:
        raise BaselineError("proxy concurrency rejection contaminated the baseline")
    if critical_events:
        raise BaselineError("ApiGate emitted a fatal log event")
    if any(record.get("level") == "error" for record in records):
        raise BaselineError("ApiGate emitted an unexpected error event")
    if not any(payload.get("event") == "service_stopped" for payload in payloads):
        raise BaselineError("ApiGate did not emit service_stopped")
    return {
        "request_budget_rejections": request_rejections,
        "response_budget_rejections": response_rejections,
        "proxy_capacity_rejections": proxy_rejections,
        "fatal_events": critical_events,
        "body_markers_absent": True,
    }


def bounded_integer(name, minimum, maximum):
    def parse(value):
        try:
            parsed = int(value, 10)
        except ValueError as error:
            raise argparse.ArgumentTypeError(
                f"{name} must be a decimal integer"
            ) from error
        if not minimum <= parsed <= maximum:
            raise argparse.ArgumentTypeError(
                f"{name} must be from {minimum} to {maximum}"
            )
        return parsed

    return parse


def bounded_float(name, minimum, maximum):
    def parse(value):
        try:
            parsed = float(value)
        except ValueError as error:
            raise argparse.ArgumentTypeError(f"{name} must be a number") from error
        if not minimum <= parsed <= maximum:
            raise argparse.ArgumentTypeError(
                f"{name} must be from {minimum} to {maximum}"
            )
        return parsed

    return parse


def parse_arguments(argv):
    parser = argparse.ArgumentParser(
        description=(
            "Measure local slow-body budget behavior and sampled ApiGate RSS. "
            "This is a correctness-oriented baseline, not a production capacity guarantee."
        ),
        epilog=(
            "Normal defaults: --rounds 3 --contenders 8 --reservation-bytes 32768 "
            "--timeout-seconds 10. --smoke changes omitted values to 1, 2, 4096, and 5."
        ),
    )
    parser.add_argument("api_gate", help="path to the built api-gate executable")
    parser.add_argument(
        "--rounds", type=bounded_integer("rounds", 1, 100), default=None
    )
    parser.add_argument(
        "--contenders", type=bounded_integer("contenders", 1, 256), default=None
    )
    parser.add_argument(
        "--reservation-bytes",
        type=bounded_integer("reservation-bytes", 1, MAX_REQUEST_BODY),
        default=None,
    )
    parser.add_argument(
        "--timeout-seconds",
        type=bounded_float("timeout-seconds", 1.0, 60.0),
        default=None,
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="use small defaults suitable for quick regression checks",
    )
    arguments = parser.parse_args(argv)
    defaults = (
        {"rounds": 1, "contenders": 2, "reservation_bytes": 4096, "timeout": 5.0}
        if arguments.smoke
        else {
            "rounds": 3,
            "contenders": 8,
            "reservation_bytes": 32768,
            "timeout": 10.0,
        }
    )
    arguments.rounds = arguments.rounds or defaults["rounds"]
    arguments.contenders = arguments.contenders or defaults["contenders"]
    arguments.reservation_bytes = (
        arguments.reservation_bytes or defaults["reservation_bytes"]
    )
    arguments.timeout_seconds = arguments.timeout_seconds or defaults["timeout"]
    return arguments


def resolve_binary(path):
    candidate = pathlib.Path(path).expanduser().resolve()
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        raise BaselineError("the ApiGate executable path is not an executable file")
    return candidate


def run_baseline(arguments):
    binary = resolve_binary(arguments.api_gate)
    upstream = BaselineUpstream(arguments.timeout_seconds * 2.0)
    service = None
    sampler = None
    sampler_stopped = False
    request_metrics = ScenarioMetrics()
    response_metrics = ScenarioMetrics()
    upstream.start()
    try:
        service = ApiGateProcess(
            binary,
            upstream.port,
            arguments.reservation_bytes,
            arguments.contenders,
            arguments.timeout_seconds,
        )
        listener = service.wait_for_event(
            "http_listener_started", arguments.timeout_seconds
        )
        port = listener.get("port")
        if not isinstance(port, int) or not 1 <= port <= 65535:
            raise BaselineError("ApiGate reported an invalid listener port")

        baseline_rss_kib = read_rss_kib(service.pid)
        sampler = RssSampler(service.pid)
        sampler.start(baseline_rss_kib)

        for _ in range(arguments.rounds):
            run_slow_request_scenario(
                port,
                arguments.reservation_bytes,
                arguments.contenders,
                arguments.timeout_seconds,
                upstream,
                service,
                request_metrics,
            )
            run_slow_response_scenario(
                port,
                arguments.reservation_bytes,
                arguments.contenders,
                arguments.timeout_seconds,
                upstream,
                service,
                response_metrics,
            )

        # End measurement while the process is still alive: a zombie has no VmRSS.
        peak_rss_kib = sampler.stop(arguments.timeout_seconds)
        sampler_stopped = True
        service.stop(arguments.timeout_seconds)
        log_verification = validate_logs(service, request_metrics, response_metrics)
        upstream_failures = upstream.state.failures()
        if upstream_failures:
            raise BaselineError("the controlled upstream reported an internal failure")

        return {
            "schema_version": 1,
            "parameters": {
                "rounds": arguments.rounds,
                "contenders": arguments.contenders,
                "reservation_bytes": arguments.reservation_bytes,
                "timeout_seconds": arguments.timeout_seconds,
                "smoke": arguments.smoke,
            },
            "scenarios": {
                "slow_request_body": request_metrics.as_json(
                    arguments.rounds, arguments.contenders
                ),
                "slow_upstream_response_body": response_metrics.as_json(
                    arguments.rounds, arguments.contenders
                ),
            },
            "rss": {
                "unit": "KiB",
                "baseline_kib": baseline_rss_kib,
                "approx_peak_kib": peak_rss_kib,
                "delta_kib": max(0, peak_rss_kib - baseline_rss_kib),
                "source": f"/proc/{service.pid}/status VmRSS periodic samples",
                "limitations": [
                    "sampled RSS is not exact allocator accounting",
                    "results are not a stable cross-machine performance threshold",
                ],
            },
            "log_verification": log_verification,
            "final_result": "passed",
        }
    finally:
        # Nested finally blocks ensure one cleanup error cannot strand other resources.
        try:
            if sampler is not None and not sampler_stopped:
                sampler.stop(arguments.timeout_seconds)
        finally:
            try:
                if service is not None:
                    service.cleanup(arguments.timeout_seconds)
            finally:
                upstream.close(arguments.timeout_seconds)


def main(argv=None):
    arguments = parse_arguments(argv)
    try:
        result = run_baseline(arguments)
    except (BaselineError, OSError, threading.BrokenBarrierError) as error:
        print(f"capacity baseline failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
