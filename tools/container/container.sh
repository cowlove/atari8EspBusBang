#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
image_name="atari8espbusbang-idf:5.5-02c5f2d-llvm-mos"
container_build_dir=".container-build"
ccache_container_dir="/root/.cache/ccache"
cache_enabled=1
while [[ "${1:-}" == "--no-cache" ]]; do
    cache_enabled=0
    shift
done
command_name="${1:-image}"

if ((cache_enabled)); then
    if [[ -z "${IDF_SOURCE_CHECKOUT:-}" ]] && command -v find >/dev/null 2>&1 && command -v git >/dev/null 2>&1; then
        candidates=(
            "${HOME}/esp/esp-idf"
            "${HOME}/opt/atari8esp-idf"
            "${HOME}/opt/esp-idf"
            "${HOME}/src/esp-idf"
        )
        while IFS= read -r git_dir; do
            candidates+=("$(dirname -- "${git_dir}")")
        done < <(find "${HOME}" -maxdepth 5 -name .git \
            -not -path "${HOME}/.cache/*" -not -path "${HOME}/.local/*" \
            -not -path "${HOME}/.openclaw/*" -print 2>/dev/null)
        for candidate in "${candidates[@]}"; do
            if [[ -d "${candidate}/.git" || -f "${candidate}/.git" ]] && \
                git -C "${candidate}" cat-file -e '02c5f2dbb95859bc4a35fb6d82bbc0784968efc5^{commit}' 2>/dev/null; then
                candidate_has_overrides=1
                while read -r revision submodule; do
                    [[ -n "${revision:-}" && -n "${submodule:-}" ]] || continue
                    if ! git -C "${candidate}/${submodule}" cat-file -e "${revision}^{commit}" 2>/dev/null; then
                        candidate_has_overrides=0
                        break
                    fi
                done < "${script_dir}/idf-submodule-overrides.txt"
                if ((candidate_has_overrides)); then
                    export IDF_SOURCE_CHECKOUT="${candidate}"
                    printf 'Auto-detected ESP-IDF Git cache: %s\n' "${IDF_SOURCE_CHECKOUT}"
                    break
                fi
            fi
        done
    fi
    if [[ -z "${IDF_DIST_CACHE:-}" && -d "${IDF_TOOLS_PATH:-${HOME}/.espressif}/dist" ]]; then
        export IDF_DIST_CACHE="${IDF_TOOLS_PATH:-${HOME}/.espressif}/dist"
        printf 'Auto-detected ESP-IDF dist cache: %s\n' "${IDF_DIST_CACHE}"
    fi
else
    unset IDF_SOURCE_CHECKOUT IDF_DIST_CACHE
    printf 'ESP-IDF source and dist-cache reuse disabled.\n'
fi

build_image() {
    local build_args=(
        --file "${script_dir}/Containerfile" \
        --tag "${image_name}" \
    )
    if ((!cache_enabled)); then
        build_args+=(--no-cache)
    fi
    if [[ -n "${IDF_SOURCE_CHECKOUT:-}" ]]; then
        local idf_source
        [[ -d "${IDF_SOURCE_CHECKOUT}" ]] || {
            printf 'IDF_SOURCE_CHECKOUT does not exist: %s\n' "${IDF_SOURCE_CHECKOUT}" >&2
            return 2
        }
        idf_source="$(cd -- "${IDF_SOURCE_CHECKOUT}" && pwd -P)"
        [[ -d "${idf_source}/.git" || -f "${idf_source}/.git" ]] || {
            printf 'IDF_SOURCE_CHECKOUT is not a Git checkout: %s\n' "${idf_source}" >&2
            return 2
        }
        build_args+=(--volume "${idf_source}:/build-cache/esp-idf:ro")
        build_args+=(--build-arg IDF_SOURCE_CHECKOUT=/build-cache/esp-idf)
    fi
    if [[ -n "${IDF_DIST_CACHE:-}" ]]; then
        local idf_dist
        idf_dist="$(mkdir -p -- "${IDF_DIST_CACHE}" && cd -- "${IDF_DIST_CACHE}" && pwd -P)"
        build_args+=(--volume "${idf_dist}:/opt/espressif/dist:rw")
    fi
    podman build "${build_args[@]}" "${script_dir}"
}

prepare_ccache_dir() {
    local host_ccache_dir="${ATARI8ESP_CCACHE_DIR:-$(ccache -k cache_dir)}"
    [[ -n "${host_ccache_dir}" ]] || {
        printf 'Could not determine the host ccache cache directory. Set ATARI8ESP_CCACHE_DIR explicitly.\n' >&2
        return 1
    }
    mkdir -p -- "${host_ccache_dir}"
    (cd -- "${host_ccache_dir}" && pwd -P)
}

ensure_host_container_runtime() {
    "${script_dir}/setup-build-env.sh" container-runtime

    local required_command
    for required_command in ccache find git usermod; do
        command -v "${required_command}" >/dev/null 2>&1 || {
            printf 'Required host command is unavailable: %s\n' "${required_command}" >&2
            return 1
        }
    done
    command -v podman >/dev/null 2>&1 || {
        printf 'Podman is still unavailable after package setup. Install it manually and retry.\n' >&2
        return 1
    }
    command -v crun >/dev/null 2>&1 || {
        printf 'crun is still unavailable after package setup. Install it manually and retry.\n' >&2
        return 1
    }
    command -v pasta >/dev/null 2>&1 || {
        printf 'pasta is still unavailable after package setup. Install the passt package and retry.\n' >&2
        return 1
    }
    podman info >/dev/null || {
        printf 'Podman is installed, but its host/rootless configuration is not usable. Configure Podman for this user, then retry.\n' >&2
        return 1
    }
}

case "${command_name}" in
    image)
        shift
        (($# == 0)) || { printf 'Usage: %s [--no-cache] [image]\n' "$0" >&2; exit 2; }
        ensure_host_container_runtime
        build_image
        ;;
    build)
        ensure_host_container_runtime
        shift
        project_dir="${repo_root}"
        if [[ $# -gt 0 && "$1" != -* ]]; then
            project_dir="$1"
            shift
        fi
        [[ "${1:-}" != "--" ]] || shift
        project_dir="$(cd -- "${project_dir}" && pwd)"
        idf_args=("$@")
        LIBSMB2_PREFIX="${project_dir}/components/libsmb2" "${script_dir}/setup-build-env.sh" libsmb2
        if ((!cache_enabled)) || ! podman image exists "${image_name}"; then
            build_image
        fi
        host_ccache_dir="$(prepare_ccache_dir)"
        podman run --rm \
            --volume "${project_dir}:/workspace" \
            --volume "${host_ccache_dir}:${ccache_container_dir}:rw" \
            --env "CCACHE_DIR=${ccache_container_dir}" \
            --workdir /workspace \
            "${image_name}" bash -c \
            'bash ./tools/updateGitH.sh && exec idf.py -B "$1" --ccache "${@:2}" build' \
            _ "${container_build_dir}" "${idf_args[@]}"
        ;;
    flash)
        ensure_host_container_runtime
        requested_serial_device="${2:-}"
        if [[ -z "${requested_serial_device}" ]]; then
            printf 'Usage: %s flash <serial-device> [project-dir]\n' "$0" >&2
            exit 2
        fi
        if [[ ! -e "${requested_serial_device}" ]]; then
            printf 'Serial device does not exist: %s\n' "${requested_serial_device}" >&2
            exit 2
        fi
        host_serial_device="$(readlink -f -- "${requested_serial_device}")"
        if [[ ! -c "${host_serial_device}" ]]; then
            printf 'Not a character device: %s\n' "${requested_serial_device}" >&2
            exit 2
        fi
        if [[ ! -r "${host_serial_device}" || ! -w "${host_serial_device}" ]]; then
            printf 'Serial device needs read/write access for this host user: %s\n' "${requested_serial_device}" >&2
            printf 'Check the device permissions and your dialout-group membership.\n' >&2
            exit 2
        fi
        serial_device="${host_serial_device}"
        if [[ "${requested_serial_device}" == /dev/serial/by-id/* ]]; then
            serial_device="${requested_serial_device}"
        fi
        shift 2
        project_dir="${repo_root}"
        if [[ $# -gt 0 && "$1" != -* ]]; then
            project_dir="$1"
            shift
        fi
        [[ "${1:-}" != "--" ]] || shift
        project_dir="$(cd -- "${project_dir}" && pwd)"
        idf_args=("$@")
        LIBSMB2_PREFIX="${project_dir}/components/libsmb2" "${script_dir}/setup-build-env.sh" libsmb2
        if ((!cache_enabled)) || ! podman image exists "${image_name}"; then
            build_image
        fi
        printf 'Passing serial device into the one-shot flash container: %s\n' "${serial_device}"
        if [[ "${serial_device}" != "${host_serial_device}" ]]; then
            printf 'Host device node: %s\n' "${host_serial_device}"
        fi
        flash_run_args=(--rm)
        if [[ "$(podman info --format '{{.Host.Security.Rootless}}')" == "true" ]]; then
            runtime_name="$(podman info --format '{{.Host.OCIRuntime.Name}}')"
            if [[ "${runtime_name}" != "crun" ]]; then
                crun_path="$(command -v crun || true)"
                if [[ -z "${crun_path}" ]]; then
                    printf 'Rootless USB serial passthrough requires crun to preserve host dialout-group access. Install crun or configure Podman to use it; refusing to use --privileged.\n' >&2
                    exit 2
                fi
                flash_run_args+=(--runtime "${crun_path}")
            fi
            flash_run_args+=(--group-add keep-groups)
        fi
        host_ccache_dir="$(prepare_ccache_dir)"
        podman run "${flash_run_args[@]}" \
            --device "${host_serial_device}:${host_serial_device}:rw" \
            --volume "${project_dir}:/workspace" \
            --volume "${host_ccache_dir}:${ccache_container_dir}:rw" \
            --env "CCACHE_DIR=${ccache_container_dir}" \
            --workdir /workspace \
            "${image_name}" bash -c \
            'if [[ "$1" != "$2" ]]; then mkdir -p "$(dirname -- "$2")" && ln -sfn "$1" "$2" || exit; fi; bash ./tools/updateGitH.sh && exec idf.py -B "$3" --ccache -p "$2" "${@:4}" flash' \
            _ "${host_serial_device}" "${serial_device}" "${container_build_dir}" "${idf_args[@]}"
        ;;
    shell)
        ensure_host_container_runtime
        project_dir="$(cd -- "${2:-${repo_root}}" && pwd)"
        LIBSMB2_PREFIX="${project_dir}/components/libsmb2" "${script_dir}/setup-build-env.sh" libsmb2
        if ((!cache_enabled)) || ! podman image exists "${image_name}"; then
            build_image
        fi
        host_ccache_dir="$(prepare_ccache_dir)"
        podman run --rm --interactive --tty \
            --volume "${project_dir}:/workspace" \
            --volume "${host_ccache_dir}:${ccache_container_dir}:rw" \
            --env "CCACHE_DIR=${ccache_container_dir}" \
            --env IDF_CCACHE_ENABLE=1 \
            --workdir /workspace \
            "${image_name}" bash
        ;;
    *)
        printf 'Usage: %s [--no-cache] [image|build [project-dir] [idf.py-args...]|flash <serial-device> [project-dir] [idf.py-args...]|shell [project-dir]]\n' "$0" >&2
        exit 2
        ;;
esac
