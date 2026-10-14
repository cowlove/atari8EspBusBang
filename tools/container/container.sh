#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
image_name="atari8espbusbang-idf:5.5-02c5f2d-llvm-mos"
container_build_dir=".container-build"

build_image() {
    local build_args=(
        --file "${script_dir}/Containerfile" \
        --tag "${image_name}" \
    )
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

ensure_host_container_runtime() {
    local missing_runtime=()
    command -v podman >/dev/null 2>&1 || missing_runtime+=(podman)
    command -v crun >/dev/null 2>&1 || missing_runtime+=(crun)
    if ((${#missing_runtime[@]} > 0)); then
        printf 'Installing missing host container runtime packages: %s\n' "${missing_runtime[*]}"
        "${script_dir}/setup-build-env.sh" container-runtime
    fi

    command -v podman >/dev/null 2>&1 || {
        printf 'Podman is still unavailable after package setup. Install it manually and retry.\n' >&2
        return 1
    }
    command -v crun >/dev/null 2>&1 || {
        printf 'crun is still unavailable after package setup. Install it manually and retry.\n' >&2
        return 1
    }
    podman info >/dev/null || {
        printf 'Podman is installed, but its host/rootless configuration is not usable. Configure Podman for this user, then retry.\n' >&2
        return 1
    }
}

case "${1:-image}" in
    image)
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
        if ! podman image exists "${image_name}"; then
            build_image
        fi
        podman run --rm \
            --volume "${project_dir}:/workspace" \
            --workdir /workspace \
            "${image_name}" bash -c \
            'bash ./tools/updateGitH.sh && exec idf.py -B "$1" "${@:2}" build' \
            _ "${container_build_dir}" "${idf_args[@]}"
        ;;
    flash)
        ensure_host_container_runtime
        serial_device="${2:-}"
        if [[ -z "${serial_device}" ]]; then
            printf 'Usage: %s flash <serial-device> [project-dir]\n' "$0" >&2
            exit 2
        fi
        if [[ ! -e "${serial_device}" ]]; then
            printf 'Serial device does not exist: %s\n' "${serial_device}" >&2
            exit 2
        fi
        serial_device="$(readlink -f -- "${serial_device}")"
        if [[ ! -c "${serial_device}" ]]; then
            printf 'Not a character device: %s\n' "${serial_device}" >&2
            exit 2
        fi
        if [[ ! -r "${serial_device}" || ! -w "${serial_device}" ]]; then
            printf 'Serial device needs read/write access for this host user: %s\n' "${serial_device}" >&2
            printf 'Check the device permissions and your dialout-group membership.\n' >&2
            exit 2
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
        if ! podman image exists "${image_name}"; then
            build_image
        fi
        printf 'Passing serial device into the one-shot flash container: %s\n' "${serial_device}"
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
        podman run "${flash_run_args[@]}" \
            --device "${serial_device}:${serial_device}:rw" \
            --volume "${project_dir}:/workspace" \
            --workdir /workspace \
            "${image_name}" bash -c \
            'bash ./tools/updateGitH.sh && exec idf.py -B "$2" -p "$1" "${@:3}" flash' \
            _ "${serial_device}" "${container_build_dir}" "${idf_args[@]}"
        ;;
    shell)
        ensure_host_container_runtime
        project_dir="$(cd -- "${2:-${repo_root}}" && pwd)"
        LIBSMB2_PREFIX="${project_dir}/components/libsmb2" "${script_dir}/setup-build-env.sh" libsmb2
        if ! podman image exists "${image_name}"; then
            build_image
        fi
        podman run --rm --interactive --tty \
            --volume "${project_dir}:/workspace" \
            --workdir /workspace \
            "${image_name}" bash
        ;;
    *)
        printf 'Usage: %s [image|build [project-dir] [idf.py-args...]|flash <serial-device> [project-dir] [idf.py-args...]|shell [project-dir]]\n' "$0" >&2
        exit 2
        ;;
esac
