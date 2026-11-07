#!/usr/bin/env bash
set -Eeuo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
arguments=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help)
            cat <<'HELP'
Usage: bash scripts/demo.sh [--smoke] [--binary PATH]
Run the local ApiGate 0.3.0 demo (Linux, Bash and Python 3 standard library).
Default: build linux-debug with one job, verify, then keep services until Ctrl+C.
--smoke        Verify and clean up automatically.
--binary PATH  Use an existing executable without building (relative to current directory).
--help         Show this help.
HELP
            exit 0
            ;;
        --smoke) arguments+=(--smoke); shift ;;
        --binary)
            if [[ $# -lt 2 || -z "$2" || "$2" == --* ]]; then
                echo '--binary requires an executable path' >&2
                exit 2
            fi
            arguments+=(--binary "$2"); shift 2
            ;;
        *) echo "unknown demo argument: $1" >&2; exit 2 ;;
    esac
done
if [[ "$(uname -s)" != Linux ]]; then
    echo 'demo requires Linux' >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo 'demo requires python3 (standard library only)' >&2
    exit 2
fi
exec python3 -u "${project_root}/examples/demo/demo.py" "${arguments[@]}"
