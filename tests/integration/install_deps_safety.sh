#!/usr/bin/env bash
set -Eeuo pipefail

if [[ "$(uname -s)" != "Linux" ]] ||
   ! command -v apt-get >/dev/null 2>&1 ||
   ! command -v dpkg-query >/dev/null 2>&1; then
    echo "Skipping installer safety test without Linux apt tools"
    exit 77
fi

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -r -- "${temporary_directory}"' EXIT
mkdir -p "${temporary_directory}/bin" "${temporary_directory}/external-vcpkg"

system_package_output=$(bash "${project_root}/scripts/install-deps.sh" \
    --print-system-packages)
expected_system_package_output=$(printf '%s\n' \
    build-essential ca-certificates clang clang-format clang-tidy cmake coreutils \
    curl git ninja-build pkg-config python3 shellcheck tar unzip zip)
[[ "${system_package_output}" == "${expected_system_package_output}" ]]

workflow_path="${project_root}/.github/workflows/linux-ci.yml"
grep -Fq 'bash scripts/install-deps.sh --print-system-packages' "${workflow_path}"
grep -Fq 'sudo apt-get -o APT::Update::Error-Mode=any update' "${workflow_path}"
expected_ci_install="sudo apt-get install -y --no-install-recommends \"\${system_packages[@]}\""
grep -Fq "${expected_ci_install}" "${workflow_path}"
echo "CI consumes the installer system package list"

cache_values=$(env -u VCPKG_ROOT -u APIGATE_VCPKG_ROOT -u VCPKG_DOWNLOADS \
    -u VCPKG_DEFAULT_BINARY_CACHE \
    HOME="${temporary_directory}/home" \
    XDG_CACHE_HOME="${temporary_directory}/xdg-cache" \
    bash -c "source \"\$1/scripts/common.sh\"; printf '%s\\n%s\\n' \
        \"\$VCPKG_DOWNLOADS\" \"\$VCPKG_DEFAULT_BINARY_CACHE\"" _ "${project_root}")
expected_cache_values=$(printf '%s\n%s' \
    "${temporary_directory}/xdg-cache/vcpkg/downloads" \
    "${temporary_directory}/xdg-cache/vcpkg/archives")
[[ "${cache_values}" == "${expected_cache_values}" ]]

override_values=$(VCPKG_DOWNLOADS="${temporary_directory}/custom-downloads" \
    VCPKG_DEFAULT_BINARY_CACHE="${temporary_directory}/custom-archives" \
    bash -c "source \"\$1/scripts/common.sh\"; printf '%s\\n%s\\n' \
        \"\$VCPKG_DOWNLOADS\" \"\$VCPKG_DEFAULT_BINARY_CACHE\"" _ "${project_root}")
expected_override_values=$(printf '%s\n%s' \
    "${temporary_directory}/custom-downloads" \
    "${temporary_directory}/custom-archives")
[[ "${override_values}" == "${expected_override_values}" ]]
echo "Cache defaults and environment overrides are correct"

cat >"${temporary_directory}/bin/dpkg-query" <<'MOCK_DPKG_QUERY'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${APIGATE_TEST_DPKG_CALLS}"
printf 'install ok installed'
MOCK_DPKG_QUERY
cat >"${temporary_directory}/bin/sudo" <<'MOCK_SUDO'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${APIGATE_TEST_SUDO_CALLS}"
exit 99
MOCK_SUDO
chmod +x "${temporary_directory}/bin/dpkg-query" "${temporary_directory}/bin/sudo"
dpkg_calls="${temporary_directory}/dpkg-calls"
sudo_calls="${temporary_directory}/sudo-calls"

checkout="${temporary_directory}/external-vcpkg"
git -C "${checkout}" init -q
git -C "${checkout}" -c user.name=ApiGateTest -c user.email=test@example.invalid \
    commit --allow-empty -m "test checkout" -q
git -C "${checkout}" remote add origin https://github.com/microsoft/vcpkg.git
original_commit=$(git -C "${checkout}" rev-parse HEAD)
original_branch=$(git -C "${checkout}" symbolic-ref HEAD)

if PATH="${temporary_directory}/bin:${PATH}" \
   APIGATE_TEST_DPKG_CALLS="${dpkg_calls}" \
   APIGATE_TEST_SUDO_CALLS="${sudo_calls}" \
   VCPKG_ROOT="${checkout}" \
   VCPKG_DOWNLOADS="${temporary_directory}/downloads" \
   VCPKG_DEFAULT_BINARY_CACHE="${temporary_directory}/archives" \
   bash "${project_root}/scripts/install-deps.sh" >"${temporary_directory}/output" 2>&1; then
    echo "installer unexpectedly accepted an external vcpkg checkout at another commit" >&2
    exit 1
fi

grep -q -- ' python3$' "${dpkg_calls}"
grep -q 'refusing to modify it' "${temporary_directory}/output"
[[ "$(git -C "${checkout}" rev-parse HEAD)" == "${original_commit}" ]]
[[ "$(git -C "${checkout}" symbolic-ref HEAD)" == "${original_branch}" ]]
[[ -z "$(git -C "${checkout}" status --porcelain)" ]]
echo "External vcpkg checkout was left unchanged"

printf '#!/usr/bin/env bash\nexit 1\n' >"${temporary_directory}/bin/dpkg-query"
if PATH="${temporary_directory}/bin:${PATH}" \
   APIGATE_TEST_SUDO_CALLS="${sudo_calls}" \
   VCPKG_ROOT="${checkout}" \
   VCPKG_DOWNLOADS="${temporary_directory}/downloads" \
   VCPKG_DEFAULT_BINARY_CACHE="${temporary_directory}/archives" \
   bash "${project_root}/scripts/install-deps.sh" >"${temporary_directory}/output" 2>&1; then
    echo "installer unexpectedly continued without system packages" >&2
    exit 1
fi
grep -q 'sudo apt-get -o APT::Update::Error-Mode=any update' "${temporary_directory}/output"
grep -q 'sudo apt-get install -y --no-install-recommends' "${temporary_directory}/output"
[[ ! -s "${sudo_calls}" ]]
[[ "$(git -C "${checkout}" rev-parse HEAD)" == "${original_commit}" ]]
echo "Missing system packages require a separate interactive install without invoking sudo"
