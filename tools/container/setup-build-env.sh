#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
phase="${1:-all}"
idf_repo="${IDF_REPO:-https://github.com/espressif/esp-idf.git}"
idf_commit="${IDF_COMMIT:-02c5f2dbb95859bc4a35fb6d82bbc0784968efc5}"
idf_prefix="${ESP_IDF_PREFIX:-${HOME}/opt/atari8esp-idf}"
idf_tools_path="${IDF_TOOLS_PATH:-${HOME}/.espressif}"
llvm_mos_prefix="${LLVM_MOS_PREFIX:-${HOME}/opt/llvm-mos-atarimax128}"
jobs="${BUILD_JOBS:-2}"
overrides_file="${IDF_SUBMODULE_OVERRIDES:-${script_dir}/idf-submodule-overrides.txt}"
patch_file="${IDF_SOURCE_PATCH:-${script_dir}/patches/spiffs-fs-by-label.patch}"
sdk_installer="${LLVM_MOS_SDK_INSTALLER:-${script_dir}/install-llvm-mos-sdk.sh}"
stage_dir="${idf_prefix}.setup-tmp"
sdk_stage="${llvm_mos_prefix}.setup-tmp"

die() {
    printf 'setup-build-env: %s\n' "$*" >&2
    exit 1
}

install_system_packages() {
    if [[ "${SKIP_SYSTEM_PACKAGES:-0}" == 1 ]]; then
        printf 'Skipping OS package installation because SKIP_SYSTEM_PACKAGES=1.\n'
        return
    fi

    [[ -r /etc/os-release ]] || die 'cannot identify Linux distribution; install prerequisites manually and set SKIP_SYSTEM_PACKAGES=1'
    . /etc/os-release
    case "${ID:-}" in
        debian|ubuntu) ;;
        *) die "automatic package installation supports Debian/Ubuntu only (found ${ID:-unknown}); install prerequisites manually and set SKIP_SYSTEM_PACKAGES=1" ;;
    esac
    command -v apt-get >/dev/null 2>&1 || die 'apt-get not found; install prerequisites manually and set SKIP_SYSTEM_PACKAGES=1'
    command -v dpkg-query >/dev/null 2>&1 || die 'dpkg-query not found; install prerequisites manually and set SKIP_SYSTEM_PACKAGES=1'

    local packages=(
        bash bison ca-certificates ccache cmake dfu-util flex g++ gcc git gperf
        libffi-dev libssl-dev libusb-1.0-0 make patch pkg-config python3
        python3-dev python3-numpy python3-pip python3-venv wget xz-utils
    )
    local missing=()
    local package
    for package in "${packages[@]}"; do
        if ! dpkg-query -W -f='${db:Status-Status}' "${package}" 2>/dev/null | grep -qx installed; then
            missing+=("${package}")
        fi
    done

    if ((${#missing[@]} == 0)); then
        printf 'All required Debian/Ubuntu packages are already installed.\n'
        return
    fi

    local apt_command=(apt-get)
    if ((EUID != 0)); then
        command -v sudo >/dev/null 2>&1 || die 'missing system packages; install them as root or install sudo, then rerun this script'
        sudo -v
        apt_command=(sudo apt-get)
    fi

    printf 'Installing missing Debian/Ubuntu packages: %s\n' "${missing[*]}"
    "${apt_command[@]}" update
    "${apt_command[@]}" install -y --no-install-recommends "${missing[@]}"
}

install_idf() {
    for command_name in git python3 wget patch; do
        command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
    done
    [[ -f "${overrides_file}" ]] || die "submodule override file not found: ${overrides_file}"
    [[ -f "${patch_file}" ]] || die "ESP-IDF patch not found: ${patch_file}"
    [[ ! -e "${idf_prefix}" ]] || die "refusing to replace existing ESP-IDF prefix: ${idf_prefix}; choose a fresh ESP_IDF_PREFIX"

    mkdir -p "$(dirname -- "${idf_prefix}")" "${idf_tools_path}"
    if [[ -e "${stage_dir}" ]]; then
        [[ -d "${stage_dir}/.git" ]] || die "incomplete setup directory exists and is not a Git checkout: ${stage_dir}"
        existing_origin="$(git -C "${stage_dir}" remote get-url origin)"
        [[ "${existing_origin}" == "${idf_repo}" ]] || die "setup directory has unexpected origin ${existing_origin}: ${stage_dir}"
        printf 'Resuming ESP-IDF setup in %s\n' "${stage_dir}"
    else
        git init -q "${stage_dir}"
        git -C "${stage_dir}" remote add origin "${idf_repo}"
    fi

    printf 'Fetching ESP-IDF %s and its pinned submodules...\n' "${idf_commit}"
    git -C "${stage_dir}" fetch --depth=1 origin "${idf_commit}"
    git -C "${stage_dir}" checkout --force --detach -q FETCH_HEAD
    actual_idf_commit="$(git -C "${stage_dir}" rev-parse HEAD)"
    [[ "${actual_idf_commit}" == "${idf_commit}" ]] || die "ESP-IDF commit mismatch: got ${actual_idf_commit}, expected ${idf_commit}"
    git -C "${stage_dir}" submodule update --init --recursive --depth=1

    while read -r revision submodule; do
        [[ -n "${revision:-}" && -n "${submodule:-}" ]] || continue
        printf 'Pinning %s to %s\n' "${submodule}" "${revision}"
        git -C "${stage_dir}/${submodule}" fetch --depth=1 origin "${revision}"
        git -C "${stage_dir}/${submodule}" checkout --detach "${revision}"
    done < "${overrides_file}"

    if git -C "${stage_dir}" apply --check "${patch_file}"; then
        git -C "${stage_dir}" apply "${patch_file}"
    elif ! git -C "${stage_dir}" apply --reverse --check "${patch_file}"; then
        die "SPIFFS patch does not apply cleanly to this ESP-IDF checkout"
    fi
    IDF_PATH="${stage_dir}" IDF_TOOLS_PATH="${idf_tools_path}" "${stage_dir}/install.sh" esp32s3
    IDF_PATH="${stage_dir}" IDF_TOOLS_PATH="${idf_tools_path}" \
        python3 "${stage_dir}/tools/idf_tools.py" install riscv32-esp-elf-gdb
    mv -- "${stage_dir}" "${idf_prefix}"
    printf 'Installed pinned ESP-IDF at %s\n' "${idf_prefix}"
}

install_llvm_mos() {
    [[ -x "${sdk_installer}" ]] || die "SDK installer not executable: ${sdk_installer}"
    [[ ! -e "${llvm_mos_prefix}" ]] || die "refusing to replace existing LLVM-MOS prefix: ${llvm_mos_prefix}; choose a fresh LLVM_MOS_PREFIX"
    mkdir -p "$(dirname -- "${llvm_mos_prefix}")"
    if [[ -e "${sdk_stage}" ]]; then
        [[ -x "${sdk_stage}/bin/mos-atari8-cart-atarimax128-clang" && \
           -s "${sdk_stage}/mos-platform/atari8-cart-atarimax128/lib/libcrt0.a" ]] || \
            die "incomplete SDK setup directory exists; inspect before removing: ${sdk_stage}"
        printf 'Resuming from completed SDK install in %s\n' "${sdk_stage}"
    else
        printf 'Installing the pinned LLVM-MOS SDK and AtariMax support...\n'
        BUILD_JOBS="${jobs}" "${sdk_installer}" "${sdk_stage}"
    fi
    mv -- "${sdk_stage}" "${llvm_mos_prefix}"
}

case "${phase}" in
    idf)
        install_system_packages
        install_idf
        printf 'Activate with: source %q/export.sh\n' "${idf_prefix}"
        ;;
    llvm-mos)
        install_system_packages
        install_llvm_mos
        printf 'Add to PATH: %q/bin\n' "${llvm_mos_prefix}"
        ;;
    all)
        install_system_packages
        install_idf
        install_llvm_mos
        printf '\nBuild environment installed. To activate it in this shell:\n'
        printf '  source %q/export.sh\n' "${idf_prefix}"
        printf '  export LLVM_MOS_BIN=%q\n' "${llvm_mos_prefix}/bin"
        printf '  export PATH="$LLVM_MOS_BIN:$PATH"\n'
        ;;
    *)
        printf 'Usage: %s [all|idf|llvm-mos]\n' "$0" >&2
        exit 2
        ;;
esac
