#!/usr/bin/env python3
"""Bounded metrics over real keep-alive sessions and controlled local backends."""
from contextlib import ExitStack
import http.client
import json
import math
import os
import re
import secrets
import subprocess
import sys

from health_check_test import backend, health, service_client, transition, HealthHandler, check_logs
from http_proxy_test import stop_service
from http_server_test import build_test_environment, ServiceProcess


class MetricsHandler(HealthHandler):
    def _handle_request(self):
        if self.path in ("/status-429", "/status-503"):
            self._send_response(int(self.path[-3:]), b"upstream")
        else:
            super()._handle_request()


def request(connection, method, path, body=None, headers=None):
    connection.request(method, path, body=body, headers=headers or {})
    response = connection.getresponse()
    return response, response.read()


def scrape(connection, marker=""):
    response, data = request(connection, "GET", "/metrics?" + marker,
                             headers={"Authorization": marker, "Cookie": marker})
    assert response.status == 200
    assert response.getheader("Content-Type") == "text/plain; version=0.0.4; charset=utf-8"
    assert response.getheader("Cache-Control") == "no-store"
    assert 0 < len(data) <= 128 * 1024 and data.endswith(b"\n") and b"\r" not in data
    text = data.decode("utf-8")
    if marker:
        assert marker not in text
    values, helps, types = {}, {}, {}
    for line in text.splitlines():
        if line.startswith("#"):
            kind, name, detail = line[2:].split(" ", 2)
            target = helps if kind == "HELP" else types
            assert kind in ("HELP", "TYPE") and name not in target
            target[name] = detail
            continue
        sample, number = line.split(" ")
        assert sample not in values
        assert re.fullmatch(r'apigate_[a-z_]+(?:\{[a-z_]+="[a-z_0-9]+"(?:,[a-z_]+="[a-z_0-9]+")*\})?', sample)
        value = float(number)
        assert math.isfinite(value) and value >= 0
        values[sample] = value
    assert set(helps) == set(types)
    assert types["apigate_request_processing_seconds"] == "summary"
    assert len(helps) == 11
    return values


def cumulative(values):
    return {key: value for key, value in values.items()
            if "_total" in key or key.startswith("apigate_request_processing_seconds_")}


def response_count(values, status):
    return values[f'apigate_http_responses_total{{status="{status}"}}']


def rejection(values, reason):
    return values[f'apigate_rejections_total{{reason="{reason}"}}']


def failures(values):
    return sum(value for key, value in values.items() if key.startswith("apigate_upstream_failures_total"))


def test_config(binary):
    for enabled in (None, "false", "true"):
        env = {} if enabled is None else {"APIGATE_METRICS_ENABLED": enabled}
        result = subprocess.run([binary, "--check-config"], env=build_test_environment(env),
                                capture_output=True, timeout=5, check=False)
        assert result.returncode == 0 and not result.stderr
        assert json.loads(result.stdout)["payload"]["metrics_enabled"] == (enabled == "true")
    for value in ("", "TRUE", "1", "false ", "private-invalid-value"):
        result = subprocess.run([binary], env=build_test_environment({"APIGATE_METRICS_ENABLED": value}),
                                capture_output=True, timeout=5, check=False)
        assert result.returncode != 0 and not result.stdout
        assert "private-invalid-value" not in result.stderr.decode()
        assert json.loads(result.stderr)["category"] == "configuration"
    result = subprocess.run([binary, "--help"], capture_output=True, timeout=5, check=False)
    assert result.returncode == 0 and b"APIGATE_METRICS_ENABLED" in result.stdout


def test_endpoint_and_counts(binary):
    marker = "private-" + secrets.token_hex(12)
    with ExitStack() as resources:
        upstream = backend(resources)
        upstream.server.RequestHandlerClass = MetricsHandler
        pool = {"host": "127.0.0.1", "port": upstream.port}
        routes = [{"prefix": "/", **pool},
                  {"prefix": "/strip", "strip_prefix": True, **pool},
                  {"prefix": "/metrics", **pool, "rate_limit": {"requests_per_second": 1, "burst": 1}}]
        for enabled in (None, "false"):
            with ExitStack() as processes:
                env = {} if enabled is None else {"APIGATE_METRICS_ENABLED": enabled}
                service, client = service_client(processes, binary, routes, **env)
                response, _ = request(client(), "POST", "/metrics", b"body")
                assert response.status == 201
                stop_service(service)
                check_logs(service)
        service, client = service_client(resources, binary, routes, APIGATE_METRICS_ENABLED="true")
        connection = client()
        initial = scrape(connection, marker)
        assert all(value == 0 for value in cumulative(initial).values())
        assert initial["apigate_active_connections"] == 1
        assert initial["apigate_active_proxies"] == initial["apigate_in_flight_body_bytes"] == 0
        assert len(initial) == 524  # 521 fixed + three slots
        assert cumulative(scrape(connection)) == cumulative(initial)
        for method, body, expected in (("POST", b"", 405), ("DELETE", None, 405),
                                       ("GET", b"nonempty", 400)):
            response, _ = request(connection, method, "/metrics", body)
            assert response.status == expected
            if expected == 405:
                assert response.getheader("Allow") == "GET"
        assert cumulative(scrape(connection)) == cumulative(initial)
        for path, status in (("/healthz", 200), ("/readyz", 200), ("/metrics-other", 201),
                             ("/strip/metrics", 201), ("/status-429", 429), ("/status-503", 503)):
            response, _ = request(connection, "GET", path)
            assert response.status == status
        while not upstream.state.requests.empty():
            upstream.state.requests.get_nowait()
        response, _ = request(connection, "POST", "/sensitive?" + marker, marker.encode(),
                              {"Authorization": marker, "Cookie": marker})
        assert response.status == 201
        received = upstream.state.requests.get(timeout=2)
        assert marker in received["path"] and received["body"] == marker.encode()
        # Local scrapes did not spend this route's initial token.
        assert request(connection, "GET", "/metrics/item")[0].status == 201
        assert request(connection, "GET", "/metrics/item")[0].status == 429
        values = scrape(connection, marker)
        assert response_count(values, 200) == 2 and response_count(values, 201) == 4
        assert response_count(values, 429) == 2 and response_count(values, 503) == 1
        assert rejection(values, "rate_limit") == 1
        assert rejection(values, "proxy_capacity") == rejection(values, "buffer_capacity") == 0
        assert failures(values) == 0
        assert values["apigate_request_processing_seconds_count"] == 9
        # Early protocol rejection has a generated status but no processing observation.
        bad = client()
        response, _ = request(bad, "POST", "/bad", b"", {"Expect": "100-continue"})
        assert response.status == 417
        # Even early metrics protocol rejection is excluded.
        response, _ = request(client(), "GET", "/metrics", b"", {"Expect": "100-continue"})
        assert response.status == 417
        after = scrape(connection)
        assert response_count(after, 417) == 1
        assert after["apigate_request_processing_seconds_count"] == 9
        assert cumulative(scrape(connection)) == cumulative(after)
        stop_service(service)
        check_logs(service, {"private input": marker})


def test_capacity_and_failure(binary):
    with ExitStack() as resources:
        upstream = backend(resources)
        pool = {"host": "127.0.0.1", "port": upstream.port}
        routes = [{"prefix": "/", **pool},
                  {"prefix": "/hold", "strip_prefix": True, **pool, "timeout_ms": 60000},
                  {"prefix": "/slow", "strip_prefix": True, **pool, "timeout_ms": 100}]
        service, client = service_client(resources, binary, routes, APIGATE_METRICS_ENABLED="true",
                                          APIGATE_MAX_CONCURRENT_PROXIES="1", APIGATE_MAX_IN_FLIGHT_BYTES="128",
                                          APIGATE_SHUTDOWN_GRACE_MS="100")
        connection, holder = client(), client()
        holder.request("POST", "/hold/blocked-no-content", body=b"x" * 64)
        assert upstream.state.block_started.wait(timeout=3)
        values = scrape(connection)
        assert values["apigate_active_connections"] == 2
        assert values["apigate_active_proxies"] == values["apigate_max_concurrent_proxies"] == 1
        assert values["apigate_in_flight_body_bytes"] == 64
        assert values["apigate_max_in_flight_body_bytes"] == 128
        assert request(connection, "GET", "/no-content")[0].status == 503
        assert rejection(scrape(connection), "proxy_capacity") == 1
        assert request(client(), "POST", "/no-content", b"x" * 129)[0].status == 503
        upstream.state.release_block.set()
        response = holder.getresponse()
        assert response.status == 204 and response.read() == b""
        assert request(connection, "GET", "/budget-known")[0].status == 503
        assert upstream.state.known_header_sent.wait(timeout=3)
        upstream.state.release_known_body.set()
        values = scrape(connection)
        assert rejection(values, "buffer_capacity") == 2
        assert values["apigate_active_proxies"] == values["apigate_in_flight_body_bytes"] == 0
        assert failures(values) == 0
        upstream.state.block_started.clear()
        upstream.state.release_block.clear()
        assert request(connection, "GET", "/slow/blocked-no-content")[0].status == 504
        values = scrape(connection)
        assert values['apigate_upstream_failures_total{stage="timeout"}'] == 1
        assert failures(values) == 1
        assert values["apigate_active_proxies"] == values["apigate_in_flight_body_bytes"] == 0
        upstream.state.release_block.set()
        # A pending business exchange is cancelled on drain, not reported as proxy failure.
        upstream.state.block_started.clear()
        upstream.state.release_block.clear()
        holder.request("GET", "/hold/blocked-no-content")
        assert upstream.state.block_started.wait(timeout=3)
        before = scrape(connection)
        assert failures(before) == 1 and before["apigate_active_proxies"] == 1
        resources.callback(upstream.state.release_block.set)
        service.terminate()
        assert service.process.wait(timeout=3) == 0
        upstream.state.release_block.set()
        service.finish_log_reader()
        records = check_logs_for_failures(service)
        assert sum(record["event"] == "http_upstream_request_failed" for record in records) == 1


def check_logs_for_failures(service):
    from http_server_test import assert_normal_log_output
    return [record["payload"] for record in assert_normal_log_output("".join(service.lines))]


def test_health_and_maximum(binary):
    with ExitStack() as resources:
        upstream = backend(resources)
        routes = [{"prefix": "/", "host": "127.0.0.1", "port": upstream.port,
                   "health_check": health("/probe-metrics", unhealthy=1, healthy=1)}]
        service, client = service_client(resources, binary, routes, APIGATE_METRICS_ENABLED="true")
        connection = client()
        key = 'apigate_upstream_available{route_index="0",upstream_index="0"}'
        assert scrape(connection)[key] == 1
        upstream.server.health.set_mode("failed")
        transition(service, "http_upstream_marked_unhealthy")
        values = scrape(connection)
        assert values[key] == 0 and failures(values) == 0
        assert request(connection, "GET", "/business")[0].status == 503
        values = scrape(connection)
        assert rejection(values, "no_healthy_upstream") == 1 and failures(values) == 0
        upstream.server.health.set_mode("healthy")
        transition(service, "http_upstream_marked_healthy")
        assert scrape(connection)[key] == 1
        assert request(connection, "GET", "/no-content")[0].status == 204
        stop_service(service)
        check_logs(service)
    with ExitStack() as resources:
        # No probes: maximal valid static configuration does not contact these backends.
        routes = [{"prefix": f"/r{index}", "upstreams": [{"host": "127.0.0.1", "port": 1}] * 16}
                  for index in range(32)]
        service, client = service_client(resources, binary, routes, APIGATE_METRICS_ENABLED="true")
        values = scrape(client())
        assert len(values) == 1033
        assert sum(key.startswith("apigate_upstream_available{") for key in values) == 512
        assert all(value == 1 for key, value in values.items() if key.startswith("apigate_upstream_available{"))
        stop_service(service)
        check_logs(service)
    with ExitStack() as resources:
        service = ServiceProcess(binary, {"APIGATE_METRICS_ENABLED": "true",
                                          "APIGATE_UPSTREAM_HOST": "127.0.0.1",
                                          "APIGATE_UPSTREAM_PORT": "1"})
        resources.callback(service.process.stdout.close)
        resources.callback(service.cleanup)
        port = service.wait_for_event("http_listener_started")["port"]
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        resources.callback(connection.close)
        assert not any(key.startswith("apigate_upstream_available{") for key in scrape(connection))
        stop_service(service)
        check_logs(service)


def main():
    binary = os.path.abspath(sys.argv[1])
    test_config(binary)
    test_endpoint_and_counts(binary)
    test_capacity_and_failure(binary)
    test_health_and_maximum(binary)
    print("metrics: endpoint, exact counters, bounded format, live capacity and health passed")


if __name__ == "__main__":
    main()
