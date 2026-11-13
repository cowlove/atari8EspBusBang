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

# Refuse to install toolchains from a shell where an ESP-IDF export.sh has been
# sourced: its IDF_PATH, IDF_PYTHON_ENV_PATH, venv python3 and tool PATH entries
# leak into ESP-IDF's install.sh/idf_tools.py and can mix versions.
active_idf_env_evidence() {
    local var entry
    for var in IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION IDF_DEACTIVATE_FILE_PATH \
        IDF_TOOLS_EXPORT_CMD IDF_TOOLS_INSTALL_CMD; do
        if [[ -n "${!var:-}" ]]; then
            printf '  %s=%s\n' "${var}" "${!var}"
        fi
    done
    local IFS=:
    for entry in ${PATH}; do
        case "${entry}" in
            */python_env/idf*|*/.espressif/tools/*|*/esp-idf*/tools|*/atari8esp-idf*/tools)
                printf '  PATH entry: %s\n' "${entry}"
                ;;
        esac
    done
    if command -v idf.py >/dev/null 2>&1; then
        printf '  idf.py on PATH: %s\n' "$(command -v idf.py)"
    fi
}

# An active ESP-IDF environment only matters to the ESP-IDF install itself:
# skip that step (with the evidence) and go on with the others, so a shell
# with IDF sourced can still add what's missing (llvm-mos, atr ...).
skip_idf_reason=""
case "${phase}" in
    idf|all)
        idf_env_evidence="$(active_idf_env_evidence)"
        if [[ -n "${idf_env_evidence}" ]]; then
            skip_idf_reason="an ESP-IDF environment is active in this shell:
${idf_env_evidence}
  To (re)install ESP-IDF, open a fresh shell (don't source export.sh or
  tools/export_build_env.sh) and run: $0 idf"
        fi
        ;;
esac
idf_repo="${IDF_REPO:-https://github.com/espressif/esp-idf.git}"
idf_source_checkout="${IDF_SOURCE_CHECKOUT:-}"
# ESP-IDF v6.1 release tag.
idf_commit="${IDF_COMMIT:-fff9895c82d744c7237be8847347bdd1b07c6643}"
idf_prefix="${ESP_IDF_PREFIX:-${HOME}/opt/atari8esp-idf-v6.1}"
idf_tools_path="${IDF_TOOLS_PATH:-${HOME}/.espressif}"
llvm_mos_prefix="${LLVM_MOS_PREFIX:-${HOME}/opt/llvm-mos-atarimax128}"
atari_tools_prefix="${ATARI_TOOLS_PREFIX:-${HOME}/opt/atari-tools}"
atari_tools_repo="${ATARI_TOOLS_REPO:-https://github.com/jhallen/atari-tools.git}"
atari_tools_commit="${ATARI_TOOLS_COMMIT:-835d5a6fc1258921949fe92400adb789c398b9c3}"
jobs="${BUILD_JOBS:-2}"
overrides_file="${IDF_SUBMODULE_OVERRIDES:-${script_dir}/idf-submodule-overrides.txt}"
sdk_installer="${LLVM_MOS_SDK_INSTALLER:-${script_dir}/install-llvm-mos-sdk.sh}"
stage_dir="${idf_prefix}.setup-tmp"
sdk_stage="${llvm_mos_prefix}.setup-tmp"
git_config_file=""

if ((cache_enabled)) && [[ -z "${idf_source_checkout}" ]] && \
    [[ "${phase}" == idf || "${phase}" == all ]]; then
    candidate_checkouts=(
        "${HOME}/esp/esp-idf"
        "${HOME}/opt/atari8esp-idf-v6.1"
        "${HOME}/opt/esp-idf-v6.1"
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
                [[ -n "${revision:-}" && -n "${submodule:-}" && "${revision}" != \#* ]] || continue
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

run_timed() {
    local description="$1"
    shift
    local started_at=$SECONDS
    printf '%s (this may take several minutes; progress can be quiet during extraction)...\n' \
        "${description}"
    "$@"
    printf 'Finished %s in %s seconds.\n' "${description}" "$((SECONDS - started_at))"
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
        # autoconf/automake: csim's atari800 checkout has no generated
        # configure; csim/a800/Makefile runs its autogen.sh.
        packages=(
            autoconf automake bash bison ca-certificates ccache cmake dfu-util flex g++ gcc git gperf
            libffi-dev libssl-dev libusb-1.0-0 make mosquitto-clients ninja-build patch pkg-config python3
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

subid_capacity() {
    local user_name="$1"
    local subid_file="$2"
    local user_id
    user_id="$(id -u "${user_name}")" || die "unknown user for subordinate ID setup: ${user_name}"
    [[ -r "${subid_file}" ]] || return 0
    awk -F: -v user_name="${user_name}" -v user_id="${user_id}" \
        '($1 == user_name || $1 == user_id) && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ { total += $3 } END { print total + 0 }' \
        "${subid_file}"
}

next_subid_start() {
    awk -F: '
        /^[[:space:]]*#/ || NF < 3 || $2 !~ /^[0-9]+$/ || $3 !~ /^[0-9]+$/ { next }
        { end = $2 + $3 - 1; if (end > max) max = end }
        END { if (max < 99999) max = 99999; print max + 1 }
    ' /etc/subuid /etc/subgid
}

configure_rootless_subids() {
    local user_name user_uid_capacity user_gid_capacity subid_start subid_end
    local usermod_command=(usermod)
    local subid_args=()
    local ranges_needed=()

    if ((EUID == 0)); then
        user_name="${SUDO_USER:-}"
        [[ -n "${user_name}" && "${user_name}" != root ]] || \
            die 'run container-runtime setup through sudo as your normal user; refusing to configure subordinate IDs for root'
    else
        user_name="$(id -un)"
    fi

    user_uid_capacity="$(subid_capacity "${user_name}" /etc/subuid)"
    user_gid_capacity="$(subid_capacity "${user_name}" /etc/subgid)"
    if ((user_uid_capacity >= 65536 && user_gid_capacity >= 65536)); then
        printf 'User %s already has subordinate UID/GID ranges (capacities %s/%s).\n' \
            "${user_name}" "${user_uid_capacity}" "${user_gid_capacity}"
        return
    fi

    command -v usermod >/dev/null 2>&1 || die 'usermod not found; install the passwd package to configure rootless subordinate IDs'
    subid_start="$(next_subid_start)"
    subid_end=$((subid_start + 65535))
    if ((user_uid_capacity < 65536)); then
        subid_args+=(--add-subuids "${subid_start}-${subid_end}")
        ranges_needed+=(UID)
    fi
    if ((user_gid_capacity < 65536)); then
        subid_args+=(--add-subgids "${subid_start}-${subid_end}")
        ranges_needed+=(GID)
    fi
    if ((EUID != 0)); then
        command -v sudo >/dev/null 2>&1 || die 'sudo is required to configure subordinate UID/GID ranges'
        usermod_command=(sudo usermod)
    fi

    printf 'Adding subordinate %s range %s-%s for %s.\n' \
        "${ranges_needed[*]}" "${subid_start}" "${subid_end}" "${user_name}"
    "${usermod_command[@]}" "${subid_args[@]}" "${user_name}"
}

install_idf() {
    if [[ -n "${skip_idf_reason}" ]]; then
        printf 'Skipping ESP-IDF: %s\n' "${skip_idf_reason}"
        return
    fi
    if [[ -e "${idf_prefix}" ]]; then
        printf 'Skipping ESP-IDF: %s already exists (choose a fresh ESP_IDF_PREFIX to install another).\n' "${idf_prefix}"
        return
    fi
    for command_name in git python3 wget; do
        command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
    done
    [[ -f "${overrides_file}" ]] || die "submodule override file not found: ${overrides_file}"

    if [[ -n "${idf_source_checkout}" ]]; then
        idf_source_checkout="$(cd -- "${idf_source_checkout}" && pwd -P)"
        git -C "${idf_source_checkout}" cat-file -e "${idf_commit}^{commit}" 2>/dev/null || \
            die "IDF_SOURCE_CHECKOUT does not contain pinned ESP-IDF commit ${idf_commit}"
        while read -r revision submodule; do
            [[ -n "${revision:-}" && -n "${submodule:-}" && "${revision}" != \#* ]] || continue
            git -C "${idf_source_checkout}/${submodule}" cat-file -e "${revision}^{commit}" 2>/dev/null || \
                die "IDF_SOURCE_CHECKOUT lacks pinned submodule commit ${revision} for ${submodule}"
        done < "${overrides_file}"

        git_config_file="$(mktemp)"
        git config --file "${git_config_file}" --add "url.file://${idf_source_checkout}.insteadOf" "${idf_repo}"
        git config --file "${git_config_file}" protocol.file.allow always
        while IFS=$'\t' read -r submodule origin; do
            [[ -n "${submodule}" && -n "${origin}" ]] || continue
            git config --file "${git_config_file}" --add \
                "url.file://${idf_source_checkout}/${submodule}.insteadOf" "${origin}"
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
        [[ -n "${revision:-}" && -n "${submodule:-}" && "${revision}" != \#* ]] || continue
        printf 'Pinning %s to %s\n' "${submodule}" "${revision}"
        git -C "${stage_dir}/${submodule}" fetch --depth=1 origin "${revision}"
        git -C "${stage_dir}/${submodule}" checkout --detach "${revision}"
    done < "${overrides_file}"

    run_timed 'Installing ESP-IDF tools for esp32s3' \
        env IDF_PATH="${stage_dir}" IDF_TOOLS_PATH="${idf_tools_path}" \
        "${stage_dir}/install.sh" esp32s3
    run_timed 'Installing riscv32-esp-elf-gdb' \
        env IDF_PATH="${stage_dir}" IDF_TOOLS_PATH="${idf_tools_path}" \
        python3 "${stage_dir}/tools/idf_tools.py" install riscv32-esp-elf-gdb
    mv -- "${stage_dir}" "${idf_prefix}"
    printf 'Installed pinned ESP-IDF at %s\n' "${idf_prefix}"
}

install_llvm_mos() {
    [[ -x "${sdk_installer}" ]] || die "SDK installer not executable: ${sdk_installer}"
    if [[ -e "${llvm_mos_prefix}" ]]; then
        printf 'Skipping LLVM-MOS: %s already exists (choose a fresh LLVM_MOS_PREFIX to install another).\n' "${llvm_mos_prefix}"
        return
    fi
    mkdir -p "$(dirname -- "${llvm_mos_prefix}")"
    if [[ -e "${sdk_stage}" ]]; then
        [[ -x "${sdk_stage}/bin/mos-atari8-cart-atarimax128-clang" && \
           -s "${sdk_stage}/mos-platform/atari8-cart-atarimax128/lib/libcrt0.a" ]] || \
            die "incomplete SDK setup directory exists; inspect before removing: ${sdk_stage}"
        printf 'Resuming from completed SDK install in %s\n' "${sdk_stage}"
    else
        run_timed 'Installing pinned LLVM-MOS SDK and AtariMax support' \
            env BUILD_JOBS="${jobs}" "${sdk_installer}" "${sdk_stage}"
    fi
    mv -- "${sdk_stage}" "${llvm_mos_prefix}"
}

install_atari_tools() {
    for command_name in git make gcc; do
        command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
    done
    if [[ -e "${atari_tools_prefix}" ]]; then
        printf 'Skipping Atari tools: %s already exists (choose a fresh ATARI_TOOLS_PREFIX to install another).\n' "${atari_tools_prefix}"
        return
    fi

    local stage_dir="${atari_tools_prefix}.setup-tmp"
    [[ ! -e "${stage_dir}" ]] || die "Atari tools setup directory exists; inspect before removing: ${stage_dir}"
    mkdir -p "$(dirname -- "${atari_tools_prefix}")"
    git init "${stage_dir}"
    git -C "${stage_dir}" remote add origin "${atari_tools_repo}"
    git -C "${stage_dir}" fetch --depth=1 origin "${atari_tools_commit}"
    git -C "${stage_dir}" checkout --detach FETCH_HEAD
    run_timed 'Building Atari disk-image tool' make -C "${stage_dir}" atr
    mkdir -p "${atari_tools_prefix}/bin"
    install -m 755 "${stage_dir}/atr" "${atari_tools_prefix}/bin/atr"
    mv -- "${stage_dir}" "${atari_tools_prefix}/source"
}

case "${phase}" in
    packages)
        install_system_packages
        ;;
    container-runtime)
        install_system_packages ccache crun findutils git passwd podman uidmap
        # Rootless networking: Podman 5 uses pasta (package passt) by default
        # and fails without it, even with slirp4netns installed. Fall back to
        # slirp4netns (Podman 4's default) only where apt has no passt.
        if ! command -v pasta >/dev/null 2>&1; then
            if apt-cache policy passt 2>/dev/null | grep -q 'Candidate: [^(]'; then
                install_system_packages passt
            elif ! command -v slirp4netns >/dev/null 2>&1; then
                install_system_packages slirp4netns
            fi
        fi
        configure_rootless_subids
        printf 'Podman, crun, rootless networking, ccache, git, find, and usermod prerequisites are installed.\n'
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
        printf 'libsmb2 is a submodule now (components/libsmb2/libsmb2): git submodule update --init\n'
        ;;
    all)
        install_system_packages
        install_idf
        install_llvm_mos
        install_atari_tools
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
