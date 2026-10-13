#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
image_name="atari8espbusbang-idf:5.5-02c5f2d"
libsmb2_commit="d67e213a5c4e7e4969fd81f0b95e4ca5831fbba1"
cache_root="${XDG_CACHE_HOME:-${HOME}/.cache}/atari8espbusbang"
libsmb2_dir="${cache_root}/libsmb2-${libsmb2_commit}"

if ! command -v podman >/dev/null 2>&1; then
    printf 'Podman is required. Install Podman and configure rootless containers first.\n' >&2
    exit 1
fi

build_image() {
    podman build \
        --file "${script_dir}/Containerfile" \
        --tag "${image_name}" \
        "${script_dir}"
}

prepare_project_dependency() {
    mkdir -p "${cache_root}"
    if [[ ! -d "${libsmb2_dir}/.git" ]]; then
        if [[ -e "${libsmb2_dir}" ]]; then
            printf 'Refusing to reuse incomplete dependency directory: %s\n' "${libsmb2_dir}" >&2
            return 1
        fi
        git init "${libsmb2_dir}"
        git -C "${libsmb2_dir}" remote add origin https://github.com/sahlberg/libsmb2.git
        git -C "${libsmb2_dir}" fetch --depth=1 origin "${libsmb2_commit}"
        git -C "${libsmb2_dir}" checkout --detach FETCH_HEAD
    fi
    actual_commit="$(git -C "${libsmb2_dir}" rev-parse HEAD)"
    if [[ "${actual_commit}" != "${libsmb2_commit}" ]]; then
        printf 'libsmb2 cache has unexpected commit %s (wanted %s)\n' "${actual_commit}" "${libsmb2_commit}" >&2
        return 1
    fi
    cp "${repo_root}/smb2.CMakeLists.txt" "${libsmb2_dir}/CMakeLists.txt"
}

case "${1:-image}" in
    image)
        build_image
        ;;
    build)
        project_dir="$(cd -- "${2:-${repo_root}}" && pwd)"
        test_hw_id="${TEST_HW_ID:-4}"
        if [[ ! "${test_hw_id}" =~ ^[0-6]$ ]]; then
            printf 'TEST_HW_ID must be an integer from 0 through 6.\n' >&2
            exit 2
        fi
        if ! podman image exists "${image_name}"; then
            build_image
        fi
        prepare_project_dependency
        podman run --rm \
            --volume "${project_dir}:/workspace" \
            --volume "${libsmb2_dir}:/workspace/components/libsmb2:ro" \
            --workdir /workspace \
            "${image_name}" bash -c "bash ./tools/updateGitH.sh && idf.py -DTEST_HW_ID=${test_hw_id} build"
        ;;
    shell)
        project_dir="$(cd -- "${2:-${repo_root}}" && pwd)"
        if ! podman image exists "${image_name}"; then
            build_image
        fi
        prepare_project_dependency
        podman run --rm --interactive --tty \
            --volume "${project_dir}:/workspace" \
            --volume "${libsmb2_dir}:/workspace/components/libsmb2:ro" \
            --workdir /workspace \
            "${image_name}" bash
        ;;
    *)
        printf 'Usage: %s [image|build [project-dir]|shell [project-dir]]\n' "$0" >&2
        exit 2
        ;;
esac
