#!/usr/bin/env bash
set -Eeuo pipefail

vcpkg_root_provided=false
if [[ -n "${VCPKG_ROOT:-}" || -n "${APIGATE_VCPKG_ROOT:-}" ]]; then
    vcpkg_root_provided=true
fi

# shellcheck source=scripts/common.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
require_linux
require_command timeout
require_command apt-get
require_command dpkg-query

system_packages=(build-essential ca-certificates clang clang-format clang-tidy cmake curl git
                 ninja-build pkg-config python3 shellcheck tar unzip zip)
missing_system_packages=()
for package in "${system_packages[@]}"; do
    package_status=$(dpkg-query -W -f='${Status}' "${package}" 2>/dev/null || true)
    if [[ "${package_status}" != "install ok installed" ]]; then
        missing_system_packages+=("${package}")
    fi
done
if ((${#missing_system_packages[@]} > 0)); then
    echo "Missing system packages. Run these commands interactively as your regular Linux user:" >&2
    echo "sudo apt-get -o APT::Update::Error-Mode=any update" >&2
    printf 'sudo apt-get install -y --no-install-recommends' >&2
    printf ' %q' "${missing_system_packages[@]}" >&2
    printf '\n' >&2
    exit 1
fi

readonly vcpkg_commit="a1cae005c39be7b18ba319fced856b68d7276271"
mkdir -p "$(dirname -- "${VCPKG_ROOT}")"
mkdir -p "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}"

vcpkg_checkout_created=false
if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
    if [[ -e "${VCPKG_ROOT}" ]] && [[ -n "$(find "${VCPKG_ROOT}" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
        echo "vcpkg path exists but is not a Git checkout: ${VCPKG_ROOT}" >&2
        exit 1
    fi
    mkdir -p "${VCPKG_ROOT}"
    git -C "${VCPKG_ROOT}" init
    git -C "${VCPKG_ROOT}" remote add origin https://github.com/microsoft/vcpkg.git
    vcpkg_checkout_created=true
elif ! git -C "${VCPKG_ROOT}" diff --quiet ||
     ! git -C "${VCPKG_ROOT}" diff --cached --quiet; then
    echo "vcpkg checkout contains local changes; refusing to overwrite them" >&2
    exit 1
fi

if [[ "$(git -C "${VCPKG_ROOT}" remote get-url origin)" != \
      "https://github.com/microsoft/vcpkg.git" ]]; then
    echo "unexpected vcpkg origin; refusing to download from an untrusted remote" >&2
    exit 1
fi

current_commit=$(git -C "${VCPKG_ROOT}" rev-parse --verify HEAD 2>/dev/null) || current_commit=
if [[ "${vcpkg_checkout_created}" == "false" && "${vcpkg_root_provided}" == "true" ]]; then
    if [[ "${current_commit}" != "${vcpkg_commit}" || ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
        echo "existing user-provided VCPKG_ROOT must already contain the pinned vcpkg commit and executable; refusing to modify it" >&2
        exit 1
    fi
else
    if [[ "${current_commit}" != "${vcpkg_commit}" ]]; then
        retry_with_backoff "vcpkg source download" 3 300 2 \
            git -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=30 \
            -C "${VCPKG_ROOT}" fetch --depth 1 origin "${vcpkg_commit}"
        git -C "${VCPKG_ROOT}" checkout --detach "${vcpkg_commit}"
        retry_with_backoff "vcpkg bootstrap" 3 300 2 \
            "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
    elif [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
        retry_with_backoff "vcpkg bootstrap" 3 300 2 \
            "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
    fi
fi

echo "Linux dependencies are ready."
echo "VCPKG_ROOT=${VCPKG_ROOT}"
echo "VCPKG_DOWNLOADS=${VCPKG_DOWNLOADS}"
echo "VCPKG_DEFAULT_BINARY_CACHE=${VCPKG_DEFAULT_BINARY_CACHE}"
