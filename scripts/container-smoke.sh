#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck source=scripts/common.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
require_linux
require_command curl
require_command docker

if ! docker info >/dev/null 2>&1; then
    echo "Docker daemon is unavailable or the current user lacks permission" >&2
    exit 1
fi

readonly image="api-gate-smoke:local-$$"
readonly container="api-gate-smoke-$$"
readonly build_network="${APIGATE_DOCKER_BUILD_NETWORK:-default}"
image_created=false
container_id=

if [[ "${build_network}" != "default" && "${build_network}" != "host" ]]; then
    echo "APIGATE_DOCKER_BUILD_NETWORK must be default or host" >&2
    exit 1
fi

cleanup() {
    local original_status=$?
    local cleanup_failed=false
    trap - EXIT
    if [[ -n "${container_id}" ]] &&
       ! docker container rm --force "${container_id}" >/dev/null; then
        echo "failed to remove the smoke-test container" >&2
        cleanup_failed=true
    fi
    container_id=
    if [[ "${image_created}" == "true" ]] &&
       ! docker image rm "${image}" >/dev/null; then
        echo "failed to remove the smoke-test image" >&2
        cleanup_failed=true
    fi
    image_created=false

    if [[ "${original_status}" -eq 0 && "${cleanup_failed}" == "true" ]]; then
        exit 1
    fi
    exit "${original_status}"
}
trap cleanup EXIT

cd "${project_root}"
if docker image inspect "${image}" >/dev/null 2>&1; then
    echo "temporary smoke-test image tag already exists: ${image}" >&2
    exit 1
fi
docker compose -f deploy/compose.yaml config --quiet
retry_with_backoff "Docker image build" 3 1800 2 \
    docker build --network "${build_network}" --file deploy/Dockerfile --tag "${image}" .
image_created=true
docker run --rm "${image}" --check-config >/dev/null

container_id=$(docker run --detach --name "${container}" --init --read-only --cap-drop ALL \
    --security-opt no-new-privileges --publish 127.0.0.1::8080 "${image}")
if [[ -z "${container_id}" ]]; then
    echo "Docker did not return the smoke-test container ID" >&2
    exit 1
fi

published_port=$(docker port "${container_id}" 8080/tcp | sed -n 's/.*:\([0-9][0-9]*\)$/\1/p' | head -n 1)
if [[ -z "${published_port}" ]]; then
    echo "failed to determine the published HTTP port" >&2
    exit 1
fi

health_status=
for _ in {1..60}; do
    health_status=$(docker inspect --format '{{.State.Health.Status}}' "${container_id}")
    if [[ "${health_status}" == "healthy" ]]; then
        break
    fi
    if [[ "$(docker inspect --format '{{.State.Running}}' "${container_id}")" != "true" ]]; then
        docker logs "${container_id}" >&2
        echo "container exited before becoming healthy" >&2
        exit 1
    fi
    sleep 1
done
if [[ "${health_status}" != "healthy" ]]; then
    docker logs "${container_id}" >&2
    echo "container did not become healthy within 60 seconds" >&2
    exit 1
fi

curl --fail --silent --show-error "http://127.0.0.1:${published_port}/healthz" >/dev/null
curl --fail --silent --show-error "http://127.0.0.1:${published_port}/readyz" >/dev/null

[[ "$(docker inspect --format '{{.Config.User}}' "${container_id}")" == "apigate:apigate" ]]
[[ "$(docker inspect --format '{{.HostConfig.ReadonlyRootfs}}' "${container_id}")" == "true" ]]
docker inspect --format '{{json .HostConfig.CapDrop}}' "${container_id}" | grep -q 'ALL'

docker stop --time 10 "${container_id}" >/dev/null
[[ "$(docker inspect --format '{{.State.ExitCode}}' "${container_id}")" == "0" ]]

container_logs=$(docker logs "${container_id}" 2>&1)
grep -q '"event":"http_listener_started"' <<<"${container_logs}"
grep -q '"event":"shutdown_signal_received"' <<<"${container_logs}"
grep -q '"event":"service_stopped"' <<<"${container_logs}"

echo "Container smoke test passed for ${image}"
