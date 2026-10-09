# syntax=docker/dockerfile:1

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG USER_UID=1000
ARG USER_GID=1000

RUN set -eux; \
    apt-get -o APT::Update::Error-Mode=any -o Acquire::Retries=3 update; \
    apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        clang-format \
        clang-tidy \
        cmake \
        coreutils \
        curl \
        gdb \
        git \
        ninja-build \
        pkg-config \
        python3 \
        shellcheck \
        tar \
        unzip \
        zip; \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    existing_user="$(getent passwd "${USER_UID}" | cut -d: -f1 || true)"; \
    if [ -n "${existing_user}" ]; then userdel "${existing_user}"; fi; \
    existing_group="$(getent group "${USER_GID}" | cut -d: -f1 || true)"; \
    if [ -n "${existing_group}" ]; then groupdel "${existing_group}"; fi; \
    groupadd --gid "${USER_GID}" vscode; \
    useradd --uid "${USER_UID}" --gid "${USER_GID}" --create-home --shell /bin/bash vscode; \
    mkdir -p \
        /home/vscode/.cache/vcpkg/archives \
        /home/vscode/.cache/vcpkg/downloads \
        /home/vscode/.local/share/apigate; \
    chown -R vscode:vscode /home/vscode

ENV VCPKG_ROOT=/home/vscode/.local/share/apigate/vcpkg \
    VCPKG_DOWNLOADS=/home/vscode/.cache/vcpkg/downloads \
    VCPKG_DEFAULT_BINARY_CACHE=/home/vscode/.cache/vcpkg/archives

USER vscode:vscode
WORKDIR /workspaces/api-gate
