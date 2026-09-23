#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck source=scripts/common.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
require_linux
require_vcpkg
require_command cmake
require_command ninja

preset="${1:-linux-debug}"
case "${preset}" in
    linux-debug|linux-release|linux-analysis) ;;
    *)
        echo "unsupported preset: ${preset}" >&2
        exit 2
        ;;
esac

cd "${project_root}"
cache_file="${project_root}/build/${preset}/CMakeCache.txt"
configure_arguments=(--preset "${preset}")
if [[ -f "${cache_file}" ]]; then
    cached_vcpkg_root=$(sed -n 's/^Z_VCPKG_ROOT_DIR:INTERNAL=//p' "${cache_file}")
    if [[ -n "${cached_vcpkg_root}" && "${cached_vcpkg_root}" != "${VCPKG_ROOT}" ]]; then
        echo "vcpkg root changed; refreshing CMake cache for ${preset}"
        configure_arguments=(--fresh --preset "${preset}")
    fi
fi

retry_with_backoff "CMake configure and dependency installation" 3 1200 2 \
    cmake "${configure_arguments[@]}"
cmake --build --preset "${preset}" --parallel
