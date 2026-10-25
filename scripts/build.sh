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

# clang-tidy and template-heavy compilation can exhaust small WSL instances.
default_jobs=4
if [[ "${preset}" == linux-analysis ]]; then
    default_jobs=3
fi
build_jobs="${CMAKE_BUILD_PARALLEL_LEVEL-${default_jobs}}"
if [[ ! "${build_jobs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
    exit 2
fi

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
echo "Building ${preset} with at most ${build_jobs} parallel jobs"
cmake --build --preset "${preset}" --parallel "${build_jobs}"
