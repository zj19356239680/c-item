#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "${script_dir}/.." && pwd)
readonly script_dir
readonly project_root
readonly default_vcpkg_root="${XDG_DATA_HOME:-${HOME}/.local/share}/apigate/vcpkg"
readonly default_vcpkg_cache_root="${XDG_CACHE_HOME:-${HOME}/.cache}/vcpkg"

export VCPKG_ROOT="${VCPKG_ROOT:-${APIGATE_VCPKG_ROOT:-${default_vcpkg_root}}}"
export VCPKG_DOWNLOADS="${VCPKG_DOWNLOADS:-${default_vcpkg_cache_root}/downloads}"
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-${default_vcpkg_cache_root}/archives}"
export script_dir project_root

retry_with_backoff() {
    if [[ $# -lt 5 ]]; then
        echo "retry_with_backoff requires: label attempts timeout initial-delay command..." >&2
        return 2
    fi

    local label=$1
    local max_attempts=$2
    local timeout_seconds=$3
    local delay_seconds=$4
    shift 4

    local attempt
    local exit_status
    for ((attempt = 1; attempt <= max_attempts; ++attempt)); do
        if timeout --foreground "${timeout_seconds}" "$@"; then
            return 0
        else
            exit_status=$?
        fi

        if ((attempt == max_attempts)); then
            echo "${label} failed after ${max_attempts} attempts (exit ${exit_status})" >&2
            return "${exit_status}"
        fi
        echo "${label} failed on attempt ${attempt}/${max_attempts}; retrying in ${delay_seconds}s" >&2
        sleep "${delay_seconds}"
        delay_seconds=$((delay_seconds * 2))
    done
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "required command not found: $1" >&2
        exit 1
    fi
}

require_linux() {
    if [[ "$(uname -s)" != "Linux" ]]; then
        echo "ApiGate scripts only support Linux." >&2
        exit 1
    fi
}

require_vcpkg() {
    if [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
        echo "vcpkg is not installed at ${VCPKG_ROOT}; run scripts/install-deps.sh" >&2
        exit 1
    fi
}
