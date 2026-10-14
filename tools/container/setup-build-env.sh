#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cache_enabled=1
phase="all"
while (($# > 0)); do
    case "$1" in
        --no-cache)
            cache_enabled=0
            shift
            ;;
        --help|-h)
            printf 'Usage: %s [--no-cache] [packages|container-runtime|all|idf|llvm-mos|atr-tools|libsmb2]\n' "$0"
            exit 0
            ;;
        *)
            phase="$1"
            shift
            break
            ;;
    esac
done
idf_repo="${IDF_REPO:-https://github.com/espressif/esp-idf.git}"
idf_source_checkout="${IDF_SOURCE_CHECKOUT:-}"
idf_commit="${IDF_COMMIT:-02c5f2dbb95859bc4a35fb6d82bbc0784968efc5}"
idf_prefix="${ESP_IDF_PREFIX:-${HOME}/opt/atari8esp-idf}"
idf_tools_path="${IDF_TOOLS_PATH:-${HOME}/.espressif}"
llvm_mos_prefix="${LLVM_MOS_PREFIX:-${HOME}/opt/llvm-mos-atarimax128}"
atari_tools_prefix="${ATARI_TOOLS_PREFIX:-${HOME}/opt/atari-tools}"
atari_tools_repo="${ATARI_TOOLS_REPO:-https://github.com/jhallen/atari-tools.git}"
atari_tools_commit="${ATARI_TOOLS_COMMIT:-835d5a6fc1258921949fe92400adb789c398b9c3}"
libsmb2_repo="${LIBSMB2_REPO:-https://github.com/sahlberg/libsmb2.git}"
libsmb2_commit="${LIBSMB2_COMMIT:-d67e213a5c4e7e4969fd81f0b95e4ca5831fbba1}"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
libsmb2_dir="${LIBSMB2_PREFIX:-${repo_root}/components/libsmb2}"
libsmb2_cmake="${repo_root}/smb2.CMakeLists.txt"
jobs="${BUILD_JOBS:-2}"
overrides_file="${IDF_SUBMODULE_OVERRIDES:-${script_dir}/idf-submodule-overrides.txt}"
patch_file="${IDF_SOURCE_PATCH:-${script_dir}/patches/spiffs-fs-by-label.patch}"
sdk_installer="${LLVM_MOS_SDK_INSTALLER:-${script_dir}/install-llvm-mos-sdk.sh}"
stage_dir="${idf_prefix}.setup-tmp"
sdk_stage="${llvm_mos_prefix}.setup-tmp"
git_config_file=""

if ((cache_enabled)) && [[ -z "${idf_source_checkout}" ]] && \
    [[ "${phase}" == idf || "${phase}" == all ]]; then
    candidate_checkouts=(
        "${HOME}/esp/esp-idf"
        "${HOME}/opt/atari8esp-idf"
        "${HOME}/opt/esp-idf"
        "${HOME}/src/esp-idf"
    )
    while IFS= read -r git_dir; do
        candidate_checkouts+=("$(dirname -- "${git_dir}")")
    done < <(find "${HOME}" -maxdepth 5 -name .git \
        -not -path "${HOME}/.cache/*" -not -path "${HOME}/.local/*" \
        -not -path "${HOME}/.openclaw/*" -print 2>/dev/null)
    for candidate in "${candidate_checkouts[@]}"; do
        if [[ -d "${candidate}/.git" || -f "${candidate}/.git" ]] && \
            git -C "${candidate}" cat-file -e "${idf_commit}^{commit}" 2>/dev/null; then
            candidate_has_overrides=1
            while read -r revision submodule; do
                [[ -n "${revision:-}" && -n "${submodule:-}" ]] || continue
                if ! git -C "${candidate}/${submodule}" cat-file -e "${revision}^{commit}" 2>/dev/null; then
                    candidate_has_overrides=0
                    break
                fi
            done < "${overrides_file}"
            if ((candidate_has_overrides)); then
                idf_source_checkout="${candidate}"
                printf 'Auto-detected ESP-IDF Git cache: %s\n' "${idf_source_checkout}"
                break
            fi
        fi
    done
fi

if ((!cache_enabled)); then
    idf_source_checkout=""
    printf 'ESP-IDF source-checkout caching disabled.\n'
fi

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

    local packages=("$@")
    if ((${#packages[@]} == 0)); then
        packages=(
            bash bison ca-certificates ccache cmake dfu-util flex g++ gcc git gperf
            libffi-dev libssl-dev libusb-1.0-0 make patch pkg-config python3
            python3-dev python3-numpy python3-pip python3-venv wget xz-utils
        )
    fi
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

    if [[ -n "${idf_source_checkout}" ]]; then
        idf_source_checkout="$(cd -- "${idf_source_checkout}" && pwd -P)"
        git -C "${idf_source_checkout}" cat-file -e "${idf_commit}^{commit}" 2>/dev/null || \
            die "IDF_SOURCE_CHECKOUT does not contain pinned ESP-IDF commit ${idf_commit}"
        while read -r revision submodule; do
            [[ -n "${revision:-}" && -n "${submodule:-}" ]] || continue
            git -C "${idf_source_checkout}/${submodule}" cat-file -e "${revision}^{commit}" 2>/dev/null || \
                die "IDF_SOURCE_CHECKOUT lacks pinned submodule commit ${revision} for ${submodule}"
        done < "${overrides_file}"

        git_config_file="$(mktemp)"
        git config --file "${git_config_file}" --add "url.${idf_source_checkout}.insteadOf" "${idf_repo}"
        git config --file "${git_config_file}" protocol.file.allow always
        while IFS=$'\t' read -r submodule origin; do
            [[ -n "${submodule}" && -n "${origin}" ]] || continue
            git config --file "${git_config_file}" --add \
                "url.${idf_source_checkout}/${submodule}.insteadOf" "${origin}"
        done < <(git -C "${idf_source_checkout}" submodule foreach --quiet --recursive \
            'printf "%s\t%s\n" "$displaypath" "$(git config --get remote.origin.url)"')
        export GIT_CONFIG_GLOBAL="${git_config_file}"
        trap '[[ -z "${git_config_file}" ]] || rm -f -- "${git_config_file}"' EXIT
        printf 'Using validated local ESP-IDF Git cache: %s\n' "${idf_source_checkout}"
    fi

    mkdir -p "$(dirname -- "${idf_prefix}")" "${idf_tools_path}"
    if [[ -e "${stage_dir}" ]]; then
        [[ -d "${stage_dir}/.git" ]] || die "incomplete setup directory exists and is not a Git checkout: ${stage_dir}"
        existing_origin="$(git -C "${stage_dir}" remote get-url origin)"
        if [[ "${existing_origin}" != "${idf_repo}" ]]; then
            die "setup directory has unexpected origin ${existing_origin}: ${stage_dir}"
        fi
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

install_atari_tools() {
    for command_name in git make gcc; do
        command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
    done
    [[ ! -e "${atari_tools_prefix}" ]] || die "refusing to replace existing Atari tools prefix: ${atari_tools_prefix}; choose a fresh ATARI_TOOLS_PREFIX"

    local stage_dir="${atari_tools_prefix}.setup-tmp"
    [[ ! -e "${stage_dir}" ]] || die "Atari tools setup directory exists; inspect before removing: ${stage_dir}"
    mkdir -p "$(dirname -- "${atari_tools_prefix}")"
    git init "${stage_dir}"
    git -C "${stage_dir}" remote add origin "${atari_tools_repo}"
    git -C "${stage_dir}" fetch --depth=1 origin "${atari_tools_commit}"
    git -C "${stage_dir}" checkout --detach FETCH_HEAD
    make -C "${stage_dir}" atr
    mkdir -p "${atari_tools_prefix}/bin"
    install -m 755 "${stage_dir}/atr" "${atari_tools_prefix}/bin/atr"
    mv -- "${stage_dir}" "${atari_tools_prefix}/source"
}

install_libsmb2() {
    command -v git >/dev/null 2>&1 || die 'required command not found: git'
    [[ -f "${libsmb2_cmake}" ]] || die "libsmb2 component CMake file not found: ${libsmb2_cmake}"

    if [[ -d "${libsmb2_dir}/.git" ]]; then
        local existing_origin
        local existing_commit
        existing_origin="$(git -C "${libsmb2_dir}" remote get-url origin)"
        existing_commit="$(git -C "${libsmb2_dir}" rev-parse HEAD)"
        [[ "${existing_origin}" == "${libsmb2_repo}" ]] || \
            die "libsmb2 directory has unexpected origin ${existing_origin}: ${libsmb2_dir}"
        [[ "${existing_commit}" == "${libsmb2_commit}" ]] || \
            die "libsmb2 directory is at ${existing_commit}, expected ${libsmb2_commit}: ${libsmb2_dir}"
    else
        if [[ -e "${libsmb2_dir}" ]]; then
            [[ -d "${libsmb2_dir}" ]] || die "libsmb2 path is not a directory: ${libsmb2_dir}"
            [[ -z "$(find "${libsmb2_dir}" -mindepth 1 -maxdepth 1 -print -quit)" ]] || \
                die "refusing to replace non-empty libsmb2 directory: ${libsmb2_dir}"
        fi

        local stage_dir="${libsmb2_dir}.setup-tmp"
        [[ ! -e "${stage_dir}" ]] || die "libsmb2 setup directory exists; inspect before removing: ${stage_dir}"
        mkdir -p "$(dirname -- "${libsmb2_dir}")"
        git init -q "${stage_dir}"
        git -C "${stage_dir}" remote add origin "${libsmb2_repo}"
        git -C "${stage_dir}" fetch --depth=1 origin "${libsmb2_commit}"
        git -C "${stage_dir}" checkout --detach -q FETCH_HEAD
        [[ "$(git -C "${stage_dir}" rev-parse HEAD)" == "${libsmb2_commit}" ]] || \
            die "libsmb2 commit mismatch in ${stage_dir}"
        if [[ -d "${libsmb2_dir}" ]]; then
            rmdir -- "${libsmb2_dir}"
        fi
        mv -- "${stage_dir}" "${libsmb2_dir}"
    fi

    install -m 644 "${libsmb2_cmake}" "${libsmb2_dir}/CMakeLists.txt"
    printf 'Installed pinned libsmb2 component at %s\n' "${libsmb2_dir}"
}

case "${phase}" in
    packages)
        install_system_packages
        ;;
    container-runtime)
        install_system_packages crun podman
        printf 'Podman and crun are installed for rootless container use and USB-device access.\n'
        ;;
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
    atr-tools)
        install_system_packages
        install_atari_tools
        printf 'Add to PATH: %q/bin\n' "${atari_tools_prefix}"
        ;;
    libsmb2)
        install_libsmb2
        ;;
    all)
        install_system_packages
        install_idf
        install_llvm_mos
        install_atari_tools
        install_libsmb2
        printf '\nBuild environment installed. To activate it in this shell:\n'
        printf '  source %q/export.sh\n' "${idf_prefix}"
        printf '  export LLVM_MOS_BIN=%q\n' "${llvm_mos_prefix}/bin"
        printf '  export PATH=%q:"$PATH"\n' "${atari_tools_prefix}/bin"
        printf '  export PATH="$LLVM_MOS_BIN:$PATH"\n'
        ;;
    *)
        printf 'Usage: %s [--no-cache] [packages|container-runtime|all|idf|llvm-mos|atr-tools|libsmb2]\n' "$0" >&2
        exit 2
        ;;
esac
