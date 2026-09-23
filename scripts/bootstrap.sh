#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"${script_dir}/install-deps.sh"
"${script_dir}/check.sh"
