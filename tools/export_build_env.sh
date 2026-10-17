#!/usr/bin/env bash

idf_env_ready() {
	[[ -n "${IDF_PATH:-}" ]] && command -v idf.py >/dev/null 2>&1 && \
		command -v esptool.py >/dev/null 2>&1 && command -v python >/dev/null 2>&1 && \
		[[ -r "${IDF_PATH}/components/spiffs/spiffsgen.py" ]]
}

if ! idf_env_ready; then
	for export_script in "${IDF_PATH:+${IDF_PATH}/export.sh}" \
		"${HOME}/opt/atari8esp-idf/export.sh" "${HOME}/esp/esp-idf/export.sh"; do
		[[ -n "${export_script}" && -r "${export_script}" ]] || continue
		. "${export_script}" >/dev/null || continue
		idf_env_ready && break
	done
fi

if ! idf_env_ready; then
	printf 'Could not initialize a usable ESP-IDF environment. Install it under ~/opt/atari8esp-idf or ~/esp/esp-idf, or source its export.sh first.\n' >&2
	return 1
fi

llvm_mos_tools=(
	mos-atari8-cart-std-clang
	mos-atari8-cart-atarimax128-clang
	mos-atari8-dos-clang
)
atarimax_runtime="mos-platform/atari8-cart-atarimax128/lib/libcrt0.a"
llvm_mos_bin="${LLVM_MOS_BIN:-}"

if [[ -z "${llvm_mos_bin}" ]]; then
	for candidate in "${HOME}/opt/llvm-mos-atarimax128/bin" "${HOME}/opt/llvm-mos/bin" "${HOME}"/opt/*/bin; do
		[[ -d "${candidate}" && -s "${candidate}/../${atarimax_runtime}" ]] || continue
		all_tools_found=true
		for tool in "${llvm_mos_tools[@]}"; do
			[[ -x "${candidate}/${tool}" ]] || all_tools_found=false
		done
		if [[ "${all_tools_found}" == true ]]; then
			llvm_mos_bin="${candidate}"
			break
		fi
	done
fi

if [[ -z "${llvm_mos_bin}" ]]; then
	printf 'Could not find the modified LLVM-MOS SDK with AtariMax 128 support under ~/opt.\n' >&2
	return 1
fi

for tool in "${llvm_mos_tools[@]}"; do
	if [[ ! -x "${llvm_mos_bin}/${tool}" ]]; then
		printf 'Required LLVM-MOS executable not found: %s/%s\n' "${llvm_mos_bin}" "${tool}" >&2
		return 1
	fi
done
if [[ ! -s "${llvm_mos_bin}/../${atarimax_runtime}" ]]; then
	printf 'LLVM-MOS SDK at %s lacks AtariMax runtime support (%s).\n' "${llvm_mos_bin}" "${atarimax_runtime}" >&2
	return 1
fi

export LLVM_MOS_BIN="${llvm_mos_bin}"
for tool_dir in "${HOME}/opt/atari-tools/bin" "${LLVM_MOS_BIN}"; do
	case ":${PATH}:" in
		*:"${tool_dir}":*) ;;
		*) PATH="${tool_dir}:${PATH}" ;;
	esac
done
export PATH

if ! command -v atr >/dev/null 2>&1; then
	printf 'Could not find atr; install it under ~/opt/atari-tools/bin.\n' >&2
	return 1
fi

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# TODO: Rename stash to bootlogs atomically across the deployed test farm.
if [[ ! -d "${project_root}/stash" ]]; then
	mkdir -p "${project_root}/stash" || return 1
fi
