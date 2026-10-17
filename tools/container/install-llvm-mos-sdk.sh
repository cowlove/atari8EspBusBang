#!/usr/bin/env bash
set -euo pipefail

sdk_version="23.3.0"
sdk_sha256="9f694497bc93a7c876d8c62325c7d74e7860346d80fbcd0555e19ba6856658c8"
sdk_repo="https://github.com/cowlove/llvm-mos-sdk.git"
sdk_commit="4c27eab05343c32994d571ab790f182817dd466d"
sdk_prefix="${1:-${HOME}/opt/llvm-mos}"
jobs="${BUILD_JOBS:-2}"

run_timed() {
    local description="$1"
    shift
    local started_at=$SECONDS
    printf '%s (this can be quiet for a while)...\n' "${description}"
    "$@"
    printf 'Finished %s in %s seconds.\n' "${description}" "$((SECONDS - started_at))"
}

for command_name in cmake git sha256sum tar wget xz; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'Required command not found: %s\n' "${command_name}" >&2
        exit 1
    fi
done

if [[ "$(uname -m)" != x86_64 ]]; then
    printf 'The pinned LLVM-MOS compiler archive currently supports x86_64 Linux only.\n' >&2
    exit 1
fi

if [[ -e "${sdk_prefix}" ]]; then
    printf 'Refusing to replace existing SDK prefix: %s\n' "${sdk_prefix}" >&2
    printf 'Choose a new, empty prefix or move the existing directory aside.\n' >&2
    exit 1
fi

parent_dir="$(dirname -- "${sdk_prefix}")"
mkdir -p "${parent_dir}"
work_dir="$(mktemp -d "${parent_dir}/.llvm-mos-build.XXXXXX")"
cleanup() {
    if [[ -n "${work_dir:-}" && -d "${work_dir}" ]]; then
        rm -rf -- "${work_dir}"
    fi
}
trap cleanup EXIT HUP INT TERM

archive="${work_dir}/llvm-mos-linux.tar.xz"
source_dir="${work_dir}/llvm-mos-sdk"
build_dir="${work_dir}/build"

run_timed 'Downloading LLVM-MOS release archive' wget \
    "https://github.com/llvm-mos/llvm-mos-sdk/releases/download/v${sdk_version}/llvm-mos-linux.tar.xz" \
    -O "${archive}"
printf '%s  %s\n' "${sdk_sha256}" "${archive}" | sha256sum --check --status \
    || { printf 'LLVM-MOS release archive checksum mismatch.\n' >&2; exit 1; }
run_timed 'Extracting LLVM-MOS release archive' tar -xJf "${archive}" -C "${work_dir}"
if [[ ! -x "${work_dir}/llvm-mos/bin/mos-clang" ]]; then
    printf 'Unexpected LLVM-MOS release archive layout.\n' >&2
    exit 1
fi

git init -q "${source_dir}"
git -C "${source_dir}" remote add origin "${sdk_repo}"
run_timed 'Fetching pinned LLVM-MOS SDK source' \
    git -C "${source_dir}" fetch --depth=1 origin "${sdk_commit}"
git -C "${source_dir}" checkout --detach -q FETCH_HEAD
actual_commit="$(git -C "${source_dir}" rev-parse HEAD)"
if [[ "${actual_commit}" != "${sdk_commit}" ]]; then
    printf 'SDK source commit mismatch: got %s, expected %s\n' "${actual_commit}" "${sdk_commit}" >&2
    exit 1
fi

atarimax_linker_script="${source_dir}/mos-platform/atari8-cart-atarimax128/lib/link.ld"
if [[ "$(grep -Fc 'BYTE(0x04)' "${atarimax_linker_script}")" != 1 ]]; then
    printf 'Unexpected AtariMax cartridge status byte in %s\n' "${atarimax_linker_script}" >&2
    exit 1
fi
sed -i 's/BYTE(0x04)/PROVIDE(__atari_cart_status = 0x04);\nBYTE(__atari_cart_status)/' \
    "${atarimax_linker_script}"

export PATH="${work_dir}/llvm-mos/bin:${PATH}"
run_timed 'Configuring LLVM-MOS SDK build' \
    cmake -S "${source_dir}" -B "${build_dir}" -G 'Unix Makefiles' \
    -DCMAKE_INSTALL_PREFIX="${work_dir}/llvm-mos" \
    -DLLVM_MOS_BOOTSTRAP_COMPILER=OFF \
    -DLLVM_MOS_BUILD_EXAMPLES=OFF
run_timed 'Building LLVM-MOS AtariMax target tests' \
    cmake --build "${build_dir}" --target test-atari8-cart-atarimax128 --parallel "${jobs}"
run_timed 'Installing LLVM-MOS SDK files' \
    cmake --build "${build_dir}" --target install --parallel "${jobs}"

if [[ ! -x "${work_dir}/llvm-mos/bin/mos-atari8-cart-atarimax128-clang" || \
      ! -s "${work_dir}/llvm-mos/mos-platform/atari8-cart-atarimax128/lib/libcrt0.a" ]]; then
    printf 'AtariMax target files missing from installed SDK.\n' >&2
    exit 1
fi

mv "${work_dir}/llvm-mos" "${sdk_prefix}"
printf 'Installed LLVM-MOS SDK %s with AtariMax 128 support at %s\n' "${sdk_version}" "${sdk_prefix}"
printf 'SDK source commit: %s\n' "${sdk_commit}"
