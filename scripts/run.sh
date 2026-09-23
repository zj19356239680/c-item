#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck source=scripts/common.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
preset="${APIGATE_BUILD_PRESET:-linux-debug}"

"${script_dir}/build.sh" "${preset}"
exec "${project_root}/build/${preset}/bin/api-gate" "$@"
