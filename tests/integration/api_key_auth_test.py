#!/usr/bin/env python3
"""Route-local opaque bearer keys, verified HTTPS and observable admission boundaries."""
from contextlib import ExitStack
import hashlib
import json
import os
import secrets
import socket
import sys
import tempfile

from downstream_tls_test import start, context, logs, run
from https_proxy_test import certificate_files
from upstream_pool_test import PoolBackend
from http_proxy_test import stop_service
from http_server_test import read_json_response, build_test_environment
from metrics_test import scrape, failures, rejection, response_count
import subprocess

AUTH_A = "APIGATE_AUTH_KEY_TEST_A"
AUTH_B = "APIGATE_AUTH_KEY_TEST_B"


def configured(binary, root, routes, keys, tls=True, port=0):
    environment = build_test_environment({"APIGATE_ROUTES": routes if isinstance(routes, str) else json.dumps(routes),
                                          "APIGATE_LISTEN_PORT": str(port), **keys})
    for reference in (AUTH_A, AUTH_B, "APIGATE_AUTH_KEY_TEST_C", "APIGATE_AUTH_KEY_TEST_D"):
        environment.pop(reference, None)
    environment.update(keys)
    if tls:
        environment.update(APIGATE_TLS_CERT_FILE=str(root / "server.pem"),
                           APIGATE_TLS_KEY_FILE=str(root / "server.key"))
    return subprocess.run([binary, "--check-config"], env=environment, capture_output=True,
                          timeout=4, check=False)


def test_config(binary, root, key_a):
    base = {"prefix": "/private", "host": "does-not-resolve.invalid", "port": 1}
    protected = {**base, "auth": {"api_key_env": AUTH_A}}
    with socket.socket() as occupied:
        occupied.bind(("127.0.0.1", 0))
        occupied.listen()
        for route in (protected, {"prefix": "/private", "upstreams": [{"host": "localhost", "port": 1}],
                                   "auth": {"api_key_env": AUTH_A}}):
            result = configured(binary, root, [route], {AUTH_A: key_a}, port=occupied.getsockname()[1])
            assert result.returncode == 0 and not result.stderr
            assert json.loads(result.stdout)["payload"]["authenticated_route_count"] == 1
            assert key_a.encode() not in result.stdout and str(root).encode() not in result.stdout
            assert hashlib.sha256(key_a.encode()).hexdigest().encode() not in result.stdout
    assert json.loads(configured(binary, root, [base], {}, tls=False).stdout)["payload"]["authenticated_route_count"] == 0
    for value in (None, "", key_a[:31], key_a * 4 + "x", key_a + " ", key_a + ",", key_a + "="):
        keys = {} if value is None else {AUTH_A: value}
        rejected = configured(binary, root, [protected], keys)
        assert rejected.returncode != 0 and not rejected.stdout
        assert json.loads(rejected.stderr)["category"] == "configuration"
        assert key_a.encode() not in rejected.stderr
        if value:
            assert value.encode() not in rejected.stderr
    for value in (key_a[:32], key_a * 4):
        assert configured(binary, root, [protected], {AUTH_A: value}).returncode == 0
    assert configured(binary, root, [protected], {AUTH_A: key_a}, tls=False).returncode != 0
    for auth in (None, False, [], {}, {"api_key_env": 1}, {"api_key_env": "APIGATE_AUTH_KEY_"},
                 {"api_key_env": "OTHER"}, {"api_key_env": "APIGATE_AUTH_KEY_lower"},
                 {"api_key_env": "APIGATE_AUTH_KEY_" + "A" * 65},
                 {"api_key_env": AUTH_A, "unknown": {}}, {"api_key_env": [{"nested": {}}]}):
        result = configured(binary, root, [{**base, "auth": auth}], {AUTH_A: key_a})
        assert result.returncode != 0 and not result.stdout
        assert json.loads(result.stderr)["category"] == "configuration"
    duplicate = '[{"prefix":"/","host":"localhost","port":1,"auth":{"api_key_env":"' + AUTH_A + '","api_key_env":"' + AUTH_A + '"}}]'
    assert configured(binary, root, duplicate, {AUTH_A: key_a}).returncode != 0
    version = run(binary, ["--version"], {"APIGATE_ROUTES": duplicate})
    assert version.returncode == 0 and not version.stderr
    help_result = run(binary, ["--help"], {})
    assert help_result.returncode == 0 and b"api_key_env" in help_result.stdout


def denied(connection, target="/private/item", authorization=None, body=None, duplicate=False):
    if duplicate:
        connection.putrequest("GET", target)
        connection.putheader("Authorization", authorization)
        connection.putheader("aUtHoRiZaTiOn", authorization)
        connection.endheaders()
    else:
        headers = {} if authorization is None else {"Authorization": authorization}
        connection.request("POST" if body is not None else "GET", target, body=body, headers=headers)
    response = connection.getresponse()
    payload = response.read()
    assert response.status == 401
    assert response.getheader("WWW-Authenticate") == 'Bearer realm="ApiGate"'
    assert response.getheader("Cache-Control") == "no-store"
    assert json.loads(payload) == {"error": {"code": "authentication_failed", "message": "authentication failed"}}
    return payload


def test_routes_and_admission(binary, root, key_a, key_b):
    with ExitStack() as resources:
        first, second = PoolBackend(resources, identity="first"), PoolBackend(resources, identity="second")
        pool = [{"host": "127.0.0.1", "port": backend.port} for backend in (first, second)]
        auth_a, auth_b = {"api_key_env": AUTH_A}, {"api_key_env": AUTH_B}
        routes = [{"prefix": "/private", "upstreams": pool, "strip_prefix": True, "auth": auth_a},
                  {"prefix": "/private/public", **pool[0], "strip_prefix": True},
                  {"prefix": "/private/child", **pool[1], "strip_prefix": True, "auth": auth_b},
                  {"prefix": "/limited", "upstreams": pool, "strip_prefix": True, "auth": auth_a,
                   "rate_limit": {"requests_per_second": 1, "burst": 1}},
                  {"prefix": "/hold", **pool[0], "strip_prefix": True, "auth": auth_a},
                  {"prefix": "/", **pool[0], "auth": auth_a}]
        service, _, client = start(resources, binary, root, APIGATE_ROUTES=json.dumps(routes),
            APIGATE_UPSTREAM_KEEP_ALIVE="true", APIGATE_MAX_CONCURRENT_PROXIES="1",
            APIGATE_MAX_IN_FLIGHT_BYTES="128", **{AUTH_A: key_a, AUTH_B: key_b})
        connection = client()
        good = "Bearer " + key_a
        initial = scrape(connection)
        denied_count = 0
        for value in (None, "", "Bearer", "Bearer ", "Basic " + key_a, "Bearer\t" + key_a,
                      "Bearer " + key_b, "Bearer " + key_a[:31], "Bearer " + key_a * 4 + "x",
                      good + "," + key_a, good + " " + key_a):
            denied(connection, authorization=value)
            denied_count += 1
        denied(connection, authorization=good, duplicate=True)
        denied_count += 1
        connection.request("POST", "/private/item?key=" + key_a, body=key_a.encode(),
                           headers={"Cookie": "key=" + key_a})
        response = connection.getresponse()
        assert response.status == 401 and key_a.encode() not in response.read()
        denied_count += 1
        assert first.state.request_count() == second.state.request_count() == 0
        assert first.accepts == second.accepts == 0
        after = scrape(connection)
        assert response_count(after, 401) == denied_count
        assert after["apigate_active_proxies"] == after["apigate_in_flight_body_bytes"] == 0
        assert failures(after) == 0
        for reason in ("rate_limit", "proxy_capacity", "buffer_capacity", "no_healthy_upstream"):
            assert rejection(after, reason) == rejection(initial, reason) == 0
        # Invalid attempts did not move round robin. Same keep-alive session is reauthenticated.
        original_socket = connection.sock
        for method, backend, body in (("GET", first, None), ("POST", second, b"x" * 64)):
            connection.request(method, "/private/item?raw=%2f", body=body,
                               headers={"Authorization": "bEaReR   " + key_a,
                                        "Connection": "keep-alive, X-Hop", "X-Hop": "discard"})
            response = connection.getresponse()
            assert response.status == 200 and response.read() == backend.identity.encode()
            assert connection.sock is original_socket
            received = backend.state.requests.get(timeout=2)
            assert received["path"] == "/item?raw=%2f" and received["body"] == (body or b"")
            assert received["host"] == f"127.0.0.1:{backend.port}"
            assert received["authorization"] is None
            denied(connection)
        connection.request("GET", "/private/item", headers={"Authorization": good})
        response = connection.getresponse()
        assert response.status == 200 and response.read() == b"first"
        assert first.state.requests.get(timeout=2)["authorization"] is None
        # More specific public rule does not inherit the parent's auth.
        public_marker = "public-authorization-marker"
        connection.request("GET", "/private/public/metrics?raw=%2f", headers={"Authorization": public_marker})
        response = connection.getresponse()
        assert response.status == 200 and response.read() == b"first"
        received = first.state.requests.get(timeout=2)
        assert received["path"] == "/metrics?raw=%2f" and received["authorization"] == public_marker
        denied(connection, "/private/child/item", good)
        connection.request("GET", "/private/child/item", headers={"Authorization": "Bearer " + key_b})
        response = connection.getresponse()
        assert response.status == 200 and response.read() == b"second"
        assert second.state.requests.get(timeout=2)["authorization"] is None
        denied(connection, "/private/healthz")  # rewritten local-looking path cannot bypass auth
        for path in ("/healthz", "/readyz"):
            assert read_json_response(connection, "GET", path)[0].status == 200
        assert scrape(connection)["apigate_active_proxies"] == 0
        denied(connection, "/metrics-other")
        # Failure does not spend the limited route's initial token; exhaustion remains 429.
        denied(connection, "/limited/item")
        connection.request("GET", "/limited/item", headers={"Authorization": good})
        response = connection.getresponse()
        assert response.status == 200 and response.read() == b"first"
        first.state.requests.get(timeout=2)
        connection.request("GET", "/limited/item", headers={"Authorization": good})
        response = connection.getresponse()
        assert response.status == 429 and json.loads(response.read())["error"]["code"] == "rate_limited"
        denied(connection, "/limited/item")  # must remain 401 even after route tokens are exhausted
        holder = client()
        holder.request("POST", "/hold/block", body=b"x" * 64, headers={"Authorization": good})
        assert first.blocked.wait(timeout=3)
        held = scrape(connection)
        assert held["apigate_active_proxies"] == 1 and held["apigate_in_flight_body_bytes"] == 64
        denied(connection, authorization="Bearer " + key_b, body=b"x" * 32)
        held = scrape(connection)
        assert held["apigate_active_proxies"] == 1 and held["apigate_in_flight_body_bytes"] == 64
        assert rejection(held, "proxy_capacity") == rejection(held, "buffer_capacity") == 0
        first.release.set()
        response = holder.getresponse()
        assert response.status == 200 and response.read() == b"first"
        first.state.requests.get(timeout=2)
        connection.request("GET", "/private/final", headers={"Authorization": good})
        response = connection.getresponse()
        assert response.status == 200 and response.read() == b"second"
        assert second.state.requests.get(timeout=2)["path"] == "/final"
        values = scrape(connection)
        assert values["apigate_active_proxies"] == values["apigate_in_flight_body_bytes"] == 0
        assert failures(values) == 0
        stop_service(service)
        logs(service, {"key A": key_a, "key B": key_b,
                       "digest A": hashlib.sha256(key_a.encode()).hexdigest(),
                       "digest B": hashlib.sha256(key_b.encode()).hexdigest(),
                       "certificate location": str(root),
                       "public header": public_marker})
        print("API key auth: unauthorized upstream count=0, per-request auth, token/round-robin order and permit/body recovery passed")


def test_key_set_configuration(binary, root, keys):
    references = list(keys)
    base = {"prefix": "/private", "host": "localhost", "port": 1}
    markers = [*keys.values(), *references,
               *(hashlib.sha256(value.encode()).hexdigest() for value in keys.values())]
    for backend in (base, {"prefix": "/private", "upstreams": [{"host": "localhost", "port": 1}]}):
        for selected in (references[:1], references):
            route = {**backend, "auth": {"api_key_envs": selected}}
            result = configured(binary, root, [route], keys)
            assert result.returncode == 0 and not result.stderr
            assert json.loads(result.stdout)["payload"]["authenticated_route_count"] == 1
            assert all(marker.encode() not in result.stdout for marker in markers)
        for settings in ({"api_key_envs": []}, {"api_key_envs": references + [references[0]]},
                         {"api_key_envs": [references[0], references[0]]},
                         {"api_key_envs": references, "api_key_env": references[0]},
                         {"api_key_envs": [[references[0]]]}, {"api_key_envs": [{}]},
                         {"api_key_envs": references, "unknown": "private-marker"}):
            result = configured(binary, root, [{**backend, "auth": settings}], keys)
            assert result.returncode != 0 and not result.stdout
            assert all(marker.encode() not in result.stderr for marker in markers)
        duplicate_values = {**keys, references[-1]: keys[references[0]]}
        route = {**backend, "auth": {"api_key_envs": references}}
        result = configured(binary, root, [route], duplicate_values)
        assert result.returncode != 0 and all(marker.encode() not in result.stderr for marker in markers)
        for reference in references:
            for invalid in (None, "", "short", "private-invalid-key-with-space-at-end "):
                changed = dict(keys)
                if invalid is None:
                    changed.pop(reference)
                else:
                    changed[reference] = invalid
                result = configured(binary, root, [route], changed)
                assert result.returncode != 0 and not result.stdout
                assert all(marker.encode() not in result.stderr for marker in markers)
        assert configured(binary, root, [route], keys, tls=False).returncode != 0
    help_result = run(binary, ["--help"], {})
    assert help_result.returncode == 0 and b"api_key_envs" in help_result.stdout


def test_staged_rotation(binary, root, key_old, key_new, unconfigured):
    markers = {"old key": key_old, "new key": key_new, "unconfigured key": unconfigured,
               "old digest": hashlib.sha256(key_old.encode()).hexdigest(),
               "new digest": hashlib.sha256(key_new.encode()).hexdigest()}
    phases = (({"api_key_env": AUTH_A}, {key_old}),
              ({"api_key_envs": [AUTH_A, AUTH_B]}, {key_old, key_new}),
              ({"api_key_envs": [AUTH_B]}, {key_new}))
    with ExitStack() as backends:
        backend = PoolBackend(backends)
        for phase, (auth, accepted) in enumerate(phases):
            with ExitStack() as resources:
                base = {"host": "127.0.0.1", "port": backend.port}
                route = {"prefix": "/private", "strip_prefix": True, "auth": auth,
                         **(base if phase == 0 else {"upstreams": [base]})}
                # Overlap also exercises the unchanged streaming/pool path after authentication.
                route["stream_response"] = phase == 1
                service, _, client = start(resources, binary, root, APIGATE_ROUTES=json.dumps([route]),
                    APIGATE_UPSTREAM_KEEP_ALIVE="true", APIGATE_MAX_IN_FLIGHT_BYTES="128",
                    **{AUTH_A: key_old, AUTH_B: key_new})
                connection = client()
                snapshot = scrape(connection)
                original_socket = connection.sock
                for index, candidate in enumerate((key_old, key_new, key_old, key_new, unconfigured, None)):
                    before = backend.state.request_count()
                    before_accepts = backend.accepts
                    if candidate not in accepted:
                        payload = denied(connection, authorization=("Bearer " + candidate) if candidate else None)
                        assert all(value.encode() not in payload for value in markers.values())
                        assert backend.state.request_count() == before
                        assert backend.accepts == before_accepts
                    else:
                        body = b"rotation-post" if index % 2 else None
                        connection.request("POST" if body else "GET", "/private/item?raw=%2f", body=body,
                                           headers={"Authorization": "Bearer " + candidate})
                        response = connection.getresponse()
                        assert response.status == 200 and response.read() == b"ok"
                        received = backend.state.requests.get(timeout=2)
                        assert received["authorization"] is None
                        assert received["path"] == "/item?raw=%2f" and received["body"] == (body or b"")
                        assert backend.state.request_count() == before + 1
                    assert connection.sock is original_socket
                values = scrape(connection)
                assert values["apigate_active_proxies"] == values["apigate_in_flight_body_bytes"] == 0
                assert failures(values) == failures(snapshot) == 0
                stop_service(service)
                logs(service, markers)
                connection.close()
    print("rotation: old-only -> old+new -> new-only across restarts; same TLS keep-alive reauth, denied upstream delta=0, credentials stripped")


def test_legacy_forwarding(binary, root):
    with ExitStack() as resources:
        backend = PoolBackend(resources)
        service, _, client = start(resources, binary, root, APIGATE_UPSTREAM_HOST="127.0.0.1",
                                    APIGATE_UPSTREAM_PORT=str(backend.port))
        connection = client()
        marker = "legacy-authorization-marker"
        connection.request("GET", "/item", headers={"Authorization": marker})
        response = connection.getresponse()
        assert response.status == 200 and response.read() == b"ok"
        assert backend.state.requests.get(timeout=2)["authorization"] == marker
        stop_service(service)
        logs(service, {"legacy header": marker})


def main():
    binary = os.path.abspath(sys.argv[1])
    key_a, key_b = secrets.token_hex(32), secrets.token_hex(32)
    with tempfile.TemporaryDirectory(prefix="apigate-auth-tls-") as directory:
        root = certificate_files(directory)
        test_config(binary, root, key_a)
        extra_keys = {AUTH_A: key_a, AUTH_B: key_b,
                      "APIGATE_AUTH_KEY_TEST_C": secrets.token_hex(32),
                      "APIGATE_AUTH_KEY_TEST_D": secrets.token_hex(32)}
        test_key_set_configuration(binary, root, extra_keys)
        test_staged_rotation(binary, root, key_a, key_b, secrets.token_hex(32))
        test_routes_and_admission(binary, root, key_a, key_b)
        test_legacy_forwarding(binary, root)
    print("route API key configuration and verified HTTPS acceptance passed")


if __name__ == "__main__":
    main()
