# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG VCPKG_COMMIT=a1cae005c39be7b18ba319fced856b68d7276271
ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_DOWNLOADS=/var/cache/vcpkg/downloads
ENV VCPKG_DEFAULT_BINARY_CACHE=/var/cache/vcpkg/archives

RUN set -eu; \
    attempt=1; \
    until timeout 300 apt-get -o APT::Update::Error-Mode=any -o Acquire::Retries=3 \
        -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 update; do \
        if [ "${attempt}" -ge 3 ]; then echo "APT index update failed after 3 attempts" >&2; exit 1; fi; \
        sleep "$((attempt * 2))"; attempt="$((attempt + 1))"; \
    done; \
    attempt=1; \
    until timeout 600 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 \
        -o Acquire::https::Timeout=30 install -y --no-install-recommends \
        build-essential ca-certificates cmake curl git ninja-build pkg-config python3 tar unzip zip; do \
        if [ "${attempt}" -ge 3 ]; then echo "APT package installation failed after 3 attempts" >&2; exit 1; fi; \
        sleep "$((attempt * 2))"; attempt="$((attempt + 1))"; \
    done; \
    rm -rf /var/lib/apt/lists/*
RUN set -eu; git init "${VCPKG_ROOT}"; git -C "${VCPKG_ROOT}" remote add origin https://github.com/microsoft/vcpkg.git; attempt=1; until timeout 300 git -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=30 -C "${VCPKG_ROOT}" fetch --depth 1 origin "${VCPKG_COMMIT}"; do if [ "${attempt}" -ge 3 ]; then echo "vcpkg source download failed after 3 attempts" >&2; exit 1; fi; sleep "$((attempt * 2))"; attempt="$((attempt + 1))"; done; git -C "${VCPKG_ROOT}" checkout --detach "${VCPKG_COMMIT}"
RUN --mount=type=cache,target=/var/cache/vcpkg set -eu; mkdir -p "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}"; attempt=1; until timeout 300 "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics; do if [ "${attempt}" -ge 3 ]; then echo "vcpkg bootstrap failed after 3 attempts" >&2; exit 1; fi; sleep "$((attempt * 2))"; attempt="$((attempt + 1))"; done

WORKDIR /source
COPY . .
RUN --mount=type=cache,target=/var/cache/vcpkg set -eu; attempt=1; until timeout 1200 cmake --preset linux-release -DVCPKG_INSTALLED_DIR=/var/cache/vcpkg/installed/linux-release; do if [ "${attempt}" -ge 3 ]; then echo "CMake configure and dependency installation failed after 3 attempts" >&2; exit 1; fi; sleep "$((attempt * 2))"; attempt="$((attempt + 1))"; done; cmake --build --preset linux-release --parallel; ctest --preset linux-release; cmake --install build/linux-release --prefix /opt/apigate

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN set -eu; \
    attempt=1; \
    until timeout 300 apt-get -o APT::Update::Error-Mode=any -o Acquire::Retries=3 \
        -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 update; do \
        if [ "${attempt}" -ge 3 ]; then echo "APT index update failed after 3 attempts" >&2; exit 1; fi; \
        sleep "$((attempt * 2))"; attempt="$((attempt + 1))"; \
    done; \
    attempt=1; \
    until timeout 600 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 \
        -o Acquire::https::Timeout=30 install -y --no-install-recommends \
        ca-certificates curl libstdc++6; do \
        if [ "${attempt}" -ge 3 ]; then echo "APT package installation failed after 3 attempts" >&2; exit 1; fi; \
        sleep "$((attempt * 2))"; attempt="$((attempt + 1))"; \
    done; \
    rm -rf /var/lib/apt/lists/*; \
    groupadd --system apigate; \
    useradd --system --gid apigate --no-create-home --shell /usr/sbin/nologin apigate

COPY --from=builder --chown=apigate:apigate /opt/apigate /opt/apigate

USER apigate:apigate
ENV APIGATE_SERVICE_NAME=api-gate APIGATE_ENVIRONMENT=production APIGATE_LOG_LEVEL=info APIGATE_LISTEN_ADDRESS=0.0.0.0 APIGATE_LISTEN_PORT=8080

EXPOSE 8080
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 CMD ["curl", "--fail", "--silent", "--show-error", "http://127.0.0.1:8080/healthz"]
ENTRYPOINT ["/opt/apigate/bin/api-gate"]
