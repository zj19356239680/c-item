#!/usr/bin/env python3

"""Verify a real partial request body times out and returns its entire reservation."""

import http.client
import json
import os
import secrets
import socket
import sys
import time
from contextlib import ExitStack

from header_timeout_test import (
    EARLY_MARGIN_SECONDS,
    LATE_MARGIN_SECONDS,
    READ_TIMEOUT_SECONDS,
)
from http_server_test import (
    ServiceProcess,
    assert_event_absent,
    assert_peer_closed,
    assert_sensitive_values_absent,
    assert_shutdown_sequence,
    read_json_response,
)


BODY_BYTES = 64 * 1024
EXPECTED_CAPACITY_ERROR = {
    "error": {"code": "gateway_overloaded", "message": "buffer capacity is exhausted"}
}


def test_body_timeout(binary):
    sensitive = {name: f"body-timeout-{name}-" + secrets.token_hex(16)
                 for name in ("query", "authorization", "cookie", "body")}
    service = ServiceProcess(binary, {"APIGATE_MAX_IN_FLIGHT_BYTES": str(BODY_BYTES)})
    try:
        with ExitStack() as resources:
            port = int(service.wait_for_event("http_listener_started")["port"])
            connected_at = time.monotonic()
            holder = resources.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            header = (
                "POST /healthz?marker=" + sensitive["query"]
                + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + str(BODY_BYTES)
                + "\r\nAuthorization: Bearer " + sensitive["authorization"]
                + "\r\nCookie: test=" + sensitive["cookie"]
                + "\r\nConnection: close\r\n\r\n"
            ).encode("ascii")
            holder.sendall(header + sensitive["body"].encode("ascii"))

            # A bounded response-driven probe, not a timing assumption: this POST
            # needs one byte. Its exact 503 plus the active_bytes log proves the
            # holder reserved the whole 64 KiB after parsing a complete header.
            deadline = time.monotonic() + 3
            while True:
                remaining = deadline - time.monotonic()
                assert remaining > 0, "partial body did not acquire its reservation"
                probe = http.client.HTTPConnection("127.0.0.1", port, timeout=remaining)
                try:
                    response, payload = read_json_response(probe, "POST", "/healthz", body=b"x")
                    if response.status == 503:
                        assert payload == EXPECTED_CAPACITY_ERROR
                        assert response.will_close
                        break
                    assert response.status == 405
                    assert payload["error"]["code"] == "method_not_allowed"
                finally:
                    probe.close()
            rejection = service.wait_for_event("http_buffer_budget_rejected", timeout=3)
            assert rejection["direction"] == "request" and rejection["status"] == 503
            assert rejection["requested_bytes"] == 1
            assert rejection["active_bytes"] == rejection["max_bytes"] == BODY_BYTES

            remaining = connected_at + READ_TIMEOUT_SECONDS + LATE_MARGIN_SECONDS - time.monotonic()
            assert remaining > 0
            assert_peer_closed(holder, timeout=remaining)
            elapsed = time.monotonic() - connected_at
            assert elapsed >= READ_TIMEOUT_SECONDS - EARLY_MARGIN_SECONDS
            timeout = service.wait_for_event("http_request_rejected", timeout=3)
            assert timeout.get("reason") == "read_timeout"

            # The local POST returns 405 only after its entire body has been read.
            # Success with exactly BODY_BYTES proves the full reservation is reusable.
            recovered = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                response, payload = read_json_response(
                    recovered, "POST", "/healthz", body=b"r" * BODY_BYTES,
                    headers={"Connection": "close"},
                )
                assert response.status == 405
                assert payload["error"]["code"] == "method_not_allowed"
            finally:
                recovered.close()
            health = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                response, payload = read_json_response(
                    health, "GET", "/healthz", headers={"Connection": "close"}
                )
                assert response.status == 200 and payload["status"] == "ok"
            finally:
                health.close()

            service.terminate()
            assert service.process.wait(timeout=5) == 0
            service.finish_log_reader()
            output = "".join(service.lines)
            records = assert_shutdown_sequence(output, "shutdown_drain_completed")
            assert_sensitive_values_absent(output, sensitive)
            assert all(record["level"] not in ("error", "critical") for record in records)
            rejections = [record["payload"] for record in records
                          if record["payload"]["event"] == "http_request_rejected"]
            assert len(rejections) == 1 and rejections[0].get("reason") == "read_timeout"
            assert "status" not in rejections[0]
            for event in ("http_response_write_failed", "http_accept_failed", "http_accept_retry_failed",
                          "shutdown_drain_timed_out", "shutdown_force_requested"):
                assert_event_absent(output, event)
            print(f"body timeout regression passed: closure_seconds={elapsed:.3f}, "
                  f"recovered_bytes={BODY_BYTES}, timeout_diagnostics=1, write_failures=0")
    finally:
        service.cleanup()
        assert service.process.poll() is not None
        assert not service.reader.is_alive()
        service.process.stdout.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: body_timeout_test.py <api-gate-binary>")
    test_body_timeout(os.path.abspath(sys.argv[1]))


if __name__ == "__main__":
    main()
