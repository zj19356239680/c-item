#!/usr/bin/env bash
set -Eeuo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: service_lifecycle.sh <api-gate-binary> [TERM|INT]" >&2
    exit 2
fi

binary=$1
signal_name=${2:-TERM}

case "${signal_name}" in
    TERM | INT) ;;
    *)
        echo "unsupported signal: ${signal_name}" >&2
        exit 2
        ;;
esac

log_file=$(mktemp)
service_pid=

cleanup() {
    if [[ -n "${service_pid}" ]] && kill -0 "${service_pid}" 2>/dev/null; then
        kill -TERM "${service_pid}" 2>/dev/null || true
        wait "${service_pid}" 2>/dev/null || true
    fi
    rm -f "${log_file}"
}
trap cleanup EXIT

APIGATE_LISTEN_PORT=0 APIGATE_SHUTDOWN_GRACE_MS=1000 "${binary}" >"${log_file}" 2>&1 &
service_pid=$!

for _ in {1..50}; do
    if grep -q '"event":"service_started"' "${log_file}"; then
        break
    fi
    if ! kill -0 "${service_pid}" 2>/dev/null; then
        cat "${log_file}" >&2
        exit 1
    fi
    sleep 0.1
done

grep -q '"event":"service_started"' "${log_file}"
kill "-${signal_name}" "${service_pid}"
wait "${service_pid}"
service_pid=

grep -q '"event":"shutdown_signal_received"' "${log_file}"
grep -q '"event":"shutdown_drain_started"' "${log_file}"
grep -q '"event":"shutdown_drain_completed"' "${log_file}"
grep -q '"event":"http_listener_stopped"' "${log_file}"
grep -q '"event":"service_stopped"' "${log_file}"
if grep -q '"event":"shutdown_drain_timed_out"' "${log_file}"; then
    echo "shutdown without sessions unexpectedly reached the drain deadline" >&2
    exit 1
fi
if grep -q '"event":"http_accept_retry_failed"' "${log_file}"; then
    echo "normal shutdown unexpectedly reported an accept retry failure" >&2
    exit 1
fi

signal_line=$(grep -n -m1 '"event":"shutdown_signal_received"' "${log_file}" | cut -d: -f1)
started_line=$(grep -n -m1 '"event":"shutdown_drain_started"' "${log_file}" | cut -d: -f1)
completed_line=$(grep -n -m1 '"event":"shutdown_drain_completed"' "${log_file}" | cut -d: -f1)
stopped_line=$(grep -n -m1 '"event":"service_stopped"' "${log_file}" | cut -d: -f1)
if ! ((signal_line < started_line && started_line < completed_line && completed_line < stopped_line)); then
    echo "shutdown drain events were emitted out of order" >&2
    exit 1
fi
