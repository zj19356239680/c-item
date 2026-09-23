#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck source=scripts/common.sh
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
require_linux
require_command clang-format
require_command python3
require_command shellcheck

cd "${project_root}"
mapfile -d '' cpp_files < <(
    find include src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0
)
clang-format --dry-run --Werror "${cpp_files[@]}"
shellcheck -x -P "${project_root}" scripts/*.sh tests/integration/*.sh
python3 -c 'import ast, pathlib, sys; [ast.parse(pathlib.Path(path).read_text(encoding="utf-8"), filename=path) for path in sys.argv[1:]]' tests/integration/*.py
