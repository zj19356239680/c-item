#!/usr/bin/env python3

"""Observe idle/slow initial headers concurrently, with the production 10s deadline."""

import errno
import http.client
import json
import os
import secrets
import select
import socket
import sys
import time
from contextlib import ExitStack

from http_server_test import (
    ServiceProcess,
    assert_event_absent,
    assert_peer_closed,
    assert_sensitive_values_absent,
    assert_shutdown_sequence,
)


READ_TIMEOUT_SECONDS = 10.0
EARLY_MARGIN_SECONDS = 2.0
LATE_MARGIN_SECONDS = 3.0
FRAGMENT_INTERVAL_SECONDS = 0.5


def observe_timeouts(idle, slow, queued, accepted_at, connected_at):
    sockets = {idle: "idle", slow: "slow", queued: "queued"}
    closed_at = {}
    received = {"idle": bytearray(), "slow": bytearray()}
    next_fragment = accepted_at + FRAGMENT_INTERVAL_SECONDS
    last_fragment_at = None
    fragment_count = 0
    deadline = accepted_at + READ_TIMEOUT_SECONDS + LATE_MARGIN_SECONDS

    while len(closed_at) < 2:
        now = time.monotonic()
        assert now < deadline, "idle or slow header exceeded the original read deadline"
        wake_at = deadline
        if "slow" not in closed_at:
            wake_at = min(wake_at, next_fragment)
        ready, _, _ = select.select(list(sockets), [], [], max(0.0, wake_at - now))
        for client in ready:
            name = sockets[client]
            observed_at = time.monotonic()
            if name == "queued":
                assert observed_at - accepted_at >= READ_TIMEOUT_SECONDS - EARLY_MARGIN_SECONDS, (
                    "backlogged health request was processed before timeout released capacity"
                )
                del sockets[client]  # Leave its HTTP response for HTTPResponse below.
                continue
            try:
                data = client.recv(4096)
            except ConnectionResetError:
                data = b""
            if data:
                received[name].extend(data)
                assert len(received[name]) <= 4096, "unexpected timeout response length"
                continue
            closed_at[name] = observed_at
            del sockets[client]
            elapsed = observed_at - connected_at[name]
            assert elapsed >= READ_TIMEOUT_SECONDS - EARLY_MARGIN_SECONDS, (
                f"{name} connection closed too early after {elapsed:.2f}s"
            )
            if received[name]:
                # The existing mapping attempts a safe 400; timeout may instead
                # close the socket before any response can be written. No 408 required.
                assert bytes(received[name]).startswith(b"HTTP/1.1 400 ")

        now = time.monotonic()
        if "slow" not in closed_at and now >= next_fragment:
            try:
                slow.sendall(b"a")
            except (BrokenPipeError, ConnectionResetError):
                # Observe the peer's EOF/reset independently; never close it here.
                pass
            else:
                fragment_count += 1
                last_fragment_at = time.monotonic()
            # This select wakeup is the simulated client's traffic cadence, not
            # a sleep used to infer acceptance or resource release. No catch-up burst.
            next_fragment = now + FRAGMENT_INTERVAL_SECONDS

    assert last_fragment_at is not None and fragment_count >= 12
    assert last_fragment_at - accepted_at >= READ_TIMEOUT_SECONDS - EARLY_MARGIN_SECONDS, (
        "slow header traffic did not continue close enough to the original deadline"
    )
    assert closed_at["slow"] - last_fragment_at < LATE_MARGIN_SECONDS, (
        "slow header closure appears to have been timed from its last fragment"
    )
    return closed_at, fragment_count


def read_health(client, close_expected=True):
    client.settimeout(3)
    with http.client.HTTPResponse(client) as response:
        response.begin()
        assert response.status == 200
        assert json.loads(response.read())["status"] == "ok"
        assert response.will_close == close_expected
    if close_expected:
        assert_peer_closed(client, timeout=3)


def verify_logs(service, sensitive):
    output = "".join(service.lines)
    records = assert_shutdown_sequence(output, "shutdown_drain_completed")
    assert_sensitive_values_absent(output, sensitive)
    payloads = [record["payload"] for record in records]
    events = [payload["event"] for payload in payloads]
    capacity = [
        payload for payload in payloads
        if payload["event"] in ("http_accept_paused_capacity", "http_accept_resumed_capacity")
    ]
    assert capacity[0]["event"] == "http_accept_paused_capacity"
    for index, payload in enumerate(capacity):
        expected_pause = index % 2 == 0
        expected = "http_accept_paused_capacity" if expected_pause else "http_accept_resumed_capacity"
        assert payload["event"] == expected, "duplicate or out-of-order capacity transition"
        assert payload["max_connections"] == 2
        assert payload["active_connections"] == (2 if expected_pause else 1)
    assert len(capacity) >= 2

    rejected = [payload for payload in payloads if payload["event"] == "http_request_rejected"]
    read_errors = [payload for payload in rejected if "error_code" in payload]
    assert len(read_errors) == 2
    # boost::beast::error::timeout is 1 in the repository's pinned dependency.
    assert all(payload["error_code"] == 1 for payload in read_errors)
    mapped_errors = [payload for payload in rejected if "reason" in payload]
    assert len(mapped_errors) == 2
    assert all(payload["reason"] == "bad_request" and payload["status"] == 400
               for payload in mapped_errors)

    # Known current diagnostic: read timeout closes the stream, then the attempted
    # 400 write can log EBADF. Record this baseline explicitly, without treating
    # other write errors, fatal events or accept errors as expected cancellations.
    write_errors = [record for record in records
                    if record["payload"]["event"] == "http_response_write_failed"]
    assert len(write_errors) <= 2
    assert all(record["payload"]["error_code"] == errno.EBADF for record in write_errors)
    assert all(record["level"] != "critical" for record in records)
    assert all(record["level"] != "error"
               or record["payload"]["event"] == "http_response_write_failed"
               for record in records)
    for event in ("http_accept_failed", "http_accept_retry_failed",
                  "shutdown_drain_timed_out", "shutdown_force_requested"):
        assert_event_absent(output, event)
    first_rejection = events.index("http_request_rejected")
    resumed = events.index("http_accept_resumed_capacity")
    completed = events.index("http_request_completed")
    assert first_rejection < resumed < completed
    completions = [payload for payload in payloads if payload["event"] == "http_request_completed"]
    assert len(completions) == 3
    assert all(payload["route"] == "healthz" and payload["status"] == 200 for payload in completions)
    completion_positions = [index for index, event in enumerate(events)
                            if event == "http_request_completed"]
    pause_positions = [index for index, event in enumerate(events)
                       if event == "http_accept_paused_capacity"]
    assert completion_positions[-2] < pause_positions[-1] < completion_positions[-1], (
        "both recovered session slots did not reach capacity again"
    )
    assert capacity[-1]["event"] == "http_accept_paused_capacity"
    drain = [payload for payload in payloads if payload["event"] == "shutdown_drain_started"]
    assert len(drain) == 1 and drain[0]["active_sessions"] == 2
    stopped = [payload for payload in payloads if payload["event"] == "service_stopped"]
    assert len(stopped) == 1 and stopped[0]["exit_code"] == 0
    return len(write_errors)


def test_header_timeouts(binary):
    service = ServiceProcess(binary, {"APIGATE_MAX_CONNECTIONS": "2"})
    sensitive = {name: f"header-{name}-" + secrets.token_hex(16)
                 for name in ("query", "authorization", "cookie")}
    try:
        with ExitStack() as resources:
            port = int(service.wait_for_event("http_listener_started")["port"])
            connected_at = {"idle": time.monotonic()}
            idle = resources.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            connected_at["slow"] = time.monotonic()
            slow = resources.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            partial = (
                "GET /healthz?marker=" + sensitive["query"]
                + " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer "
                + sensitive["authorization"] + "\r\nCookie: test=" + sensitive["cookie"]
                + "\r\nX-Pace: "
            ).encode("ascii")
            # No header-ending blank line, body, invalid byte, or local shutdown.
            slow.sendall(partial)
            paused = service.wait_for_event("http_accept_paused_capacity")
            accepted_at = time.monotonic()
            assert paused["active_connections"] == paused["max_connections"] == 2
            # Even at the upper deadline, paced one-byte additions remain far below 8 KiB.
            assert len(partial) + 1 + int((READ_TIMEOUT_SECONDS + LATE_MARGIN_SECONDS)
                                         / FRAGMENT_INTERVAL_SECONDS) < 8192

            queued = resources.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            queued.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            ready, _, _ = select.select([queued], [], [], 0.25)
            assert not ready, "a queued connection was served while both session slots were held"

            closed_at, fragments = observe_timeouts(idle, slow, queued, accepted_at, connected_at)
            resumed = service.wait_for_event("http_accept_resumed_capacity", timeout=3)
            assert resumed["active_connections"] == 1 and resumed["max_connections"] == 2
            read_health(queued)
            fresh_clients = []
            for _ in range(2):
                fresh = resources.enter_context(
                    socket.create_connection(("127.0.0.1", port), timeout=3)
                )
                fresh.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
                read_health(fresh, close_expected=False)
                fresh_clients.append(fresh)

            service.terminate()
            assert service.process.wait(timeout=5) == 0
            for fresh in fresh_clients:
                assert_peer_closed(fresh, timeout=1)
            service.finish_log_reader()
            write_errors = verify_logs(service, sensitive)
            elapsed = {name: round(closed_at[name] - connected_at[name], 3) for name in closed_at}
            print(f"header timeout regression passed: closure_seconds={elapsed}, "
                  f"paced_fragments={fragments}, known_timeout_write_diagnostics={write_errors}")
    finally:
        service.cleanup()
        assert service.process.poll() is not None
        assert not service.reader.is_alive(), "service log reader remained active"
        service.process.stdout.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: header_timeout_test.py <api-gate-binary>")
    test_header_timeouts(os.path.abspath(sys.argv[1]))


if __name__ == "__main__":
    main()
