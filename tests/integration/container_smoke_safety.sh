#!/usr/bin/env bash
set -Eeuo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -r -- "${temporary_directory}"' EXIT
mkdir -p "${temporary_directory}/bin"

printf '#!/usr/bin/env bash\nprintf "Linux\\n"\n' >"${temporary_directory}/bin/uname"
cat >"${temporary_directory}/bin/curl" <<'MOCK_CURL'
#!/usr/bin/env bash
if [[ "${APIGATE_TEST_SCENARIO:-}" == "cleanup_failure" ]]; then
    exit 42
fi
exit 0
MOCK_CURL
cat >"${temporary_directory}/bin/docker" <<'MOCK_DOCKER'
#!/usr/bin/env bash
set -Eeuo pipefail
printf '%s\n' "$*" >>"${APIGATE_TEST_CALLS}"
case "$1" in
    info | compose | build | stop)
        exit 0
        ;;
    image)
        [[ "$2" != "inspect" ]]
        ;;
    run)
        if [[ " $* " == *" --detach "* ]]; then
            if [[ "${APIGATE_TEST_SCENARIO}" == "collision" ]]; then
                exit 19
            fi
            if [[ " $* " == *" --rm "* ]]; then
                echo "detached smoke-test container must not use --rm" >&2
                exit 20
            fi
            printf 'test-container-id\n'
        fi
        ;;
    port)
        printf '127.0.0.1:49152\n'
        ;;
    inspect)
        case "$3" in
            '{{.State.Health.Status}}') printf 'healthy\n' ;;
            '{{.State.Running}}' | '{{.HostConfig.ReadonlyRootfs}}') printf 'true\n' ;;
            '{{.Config.User}}') printf 'apigate:apigate\n' ;;
            '{{json .HostConfig.CapDrop}}') printf '["ALL"]\n' ;;
            '{{.State.ExitCode}}') printf '0\n' ;;
            *) exit 99 ;;
        esac
        ;;
    logs)
        printf '%s\n' \
            '{"event":"http_listener_started"}' \
            '{"event":"shutdown_signal_received"}' \
            '{"event":"service_stopped"}'
        ;;
    container)
        if [[ "${APIGATE_TEST_SCENARIO}" == "cleanup_failure" ]]; then
            exit 23
        fi
        [[ "$2" == "rm" && "$4" == "test-container-id" ]]
        ;;
    *)
        exit 99
        ;;
esac
MOCK_DOCKER
chmod +x "${temporary_directory}/bin/"*

calls="${temporary_directory}/calls"
if PATH="${temporary_directory}/bin:${PATH}" APIGATE_TEST_CALLS="${calls}" \
   APIGATE_TEST_SCENARIO=collision \
   bash "${project_root}/scripts/container-smoke.sh" >"${temporary_directory}/output" 2>&1; then
    echo "smoke test unexpectedly succeeded when Docker rejected the container name" >&2
    exit 1
fi
if grep -q '^container rm ' "${calls}"; then
    echo "smoke test attempted to remove a container it did not create" >&2
    exit 1
fi

: >"${calls}"
PATH="${temporary_directory}/bin:${PATH}" APIGATE_TEST_CALLS="${calls}" \
    APIGATE_TEST_SCENARIO=success \
    bash "${project_root}/scripts/container-smoke.sh" >"${temporary_directory}/output" 2>&1
one_shot_line=$(grep '^run --rm .* --check-config$' "${calls}")
detached_line=$(grep '^run --detach ' "${calls}")
[[ -n "${one_shot_line}" ]]
[[ " ${detached_line} " != *" --rm "* ]]
grep -q '^container rm --force test-container-id$' "${calls}"
stop_line=$(grep -n '^stop --time 10 test-container-id$' "${calls}" | cut -d: -f1)
exit_code_line=$(grep -n '^inspect --format {{.State.ExitCode}} test-container-id$' "${calls}" |
    cut -d: -f1)
logs_line=$(grep -n '^logs test-container-id$' "${calls}" | cut -d: -f1)
remove_line=$(grep -n '^container rm --force test-container-id$' "${calls}" | cut -d: -f1)
[[ "${stop_line}" -lt "${exit_code_line}" ]]
[[ "${exit_code_line}" -lt "${logs_line}" ]]
[[ "${logs_line}" -lt "${remove_line}" ]]
[[ "$(grep -c '^container rm --force test-container-id$' "${calls}")" -eq 1 ]]
echo "Container smoke cleanup used only its created ID after log assertions"

: >"${calls}"
cleanup_status=0
PATH="${temporary_directory}/bin:${PATH}" APIGATE_TEST_CALLS="${calls}" \
    APIGATE_TEST_SCENARIO=cleanup_failure \
    bash "${project_root}/scripts/container-smoke.sh" >"${temporary_directory}/output" 2>&1 ||
    cleanup_status=$?
[[ "${cleanup_status}" -eq 42 ]]
[[ "$(grep -c '^container rm --force test-container-id$' "${calls}")" -eq 1 ]]
grep -q 'failed to remove the smoke-test container' "${temporary_directory}/output"
echo "Cleanup failure did not mask the original smoke-test failure"
