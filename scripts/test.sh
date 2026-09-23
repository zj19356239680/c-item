#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck source=scripts/common.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
preset="${1:-linux-debug}"

"${script_dir}/build.sh" "${preset}"
cd "${project_root}"
ctest --preset "${preset}"
