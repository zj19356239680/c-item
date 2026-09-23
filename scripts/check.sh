#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck source=scripts/common.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
"${script_dir}/format-check.sh"
"${script_dir}/test.sh" linux-analysis
