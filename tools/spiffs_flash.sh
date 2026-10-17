#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/export_boot_env.sh
. ./tools/export_build_env.sh

llvm_mos_tools=(
	mos-atari8-cart-std-clang
	mos-atari8-cart-atarimax128-clang
	mos-atari8-dos-clang
)
atarimax_runtime="mos-platform/atari8-cart-atarimax128/lib/libcrt0.a"
llvm_mos_bin="${LLVM_MOS_BIN:-}"
if [[ -z "${llvm_mos_bin}" ]]; then
	for candidate in "${HOME}/opt/llvm-mos-atarimax128/bin" "${HOME}/opt/llvm-mos/bin" "${HOME}"/opt/*/bin; do
		[[ -d "${candidate}" ]] || continue
		[[ -s "${candidate}/../${atarimax_runtime}" ]] || continue
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
	printf 'Could not find all required LLVM-MOS binaries. Set LLVM_MOS_BIN to their bin directory or install them under ~/opt.\n' >&2
	exit 1
fi

for tool in "${llvm_mos_tools[@]}"; do
	if [[ ! -x "${llvm_mos_bin}/${tool}" ]]; then
		printf 'Required LLVM-MOS executable not found: %s/%s\n' "${llvm_mos_bin}" "${tool}" >&2
		exit 1
	fi
done
if [[ ! -s "${llvm_mos_bin}/../${atarimax_runtime}" ]]; then
	printf 'LLVM-MOS SDK at %s lacks AtariMax runtime support (%s).\n' "${llvm_mos_bin}" "${atarimax_runtime}" >&2
	exit 1
fi

if ! command -v atr >/dev/null 2>&1; then
	printf 'Required command not found: atr. Run ./tools/container/setup-build-env.sh atr-tools and add ~/opt/atari-tools/bin to PATH.\n' >&2
	exit 1
fi

ensure_disk_image() {
	local image="$1"
	if [[ ! -e "${image}" && ! -L "${image}" ]]; then
		printf 'Creating blank DOS 2.0 double-density disk image: %s\n' "${image}"
		atr "${image}" mkfs dos2.0d
	fi
}

ensure_disk_image ./main/lfs/d1.atr
ensure_disk_image ./main/lfs/d2.atr
ensure_disk_image ./main/lfs/llvm_d1.atr

./tools/power_off.sh
atr ./main/lfs/d1.atr put -l ./main/lfs/x2.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x256.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x192.cmd
atr ./main/lfs/d2.atr put -l ./main/lfs/x.bat

"${llvm_mos_bin}/mos-atari8-cart-std-clang" ./main/lfs/simplemem.c -Oz -o ./main/lfs/simplemem.rom
"${llvm_mos_bin}/mos-atari8-cart-std-clang" ./main/lfs/hello.c -Oz -o ./main/lfs/hello.rom
"${llvm_mos_bin}/mos-atari8-cart-atarimax128-clang" ./main/lfs/stress.c -Oz -o ./main/lfs/stress.rom
python3 ./tools/make_cart.py ./main/lfs/stress.rom ./main/lfs/stress.car
rm ./main/lfs/stress.rom
"${llvm_mos_bin}/mos-atari8-dos-clang" ./main/lfs/hello.c -Oz -o ./main/lfs/hello.exe
atr ./main/lfs/llvm_d1.atr put ./main/lfs/hello.exe autorun.sys
atr ./main/lfs/d2.atr put ./main/lfs/hello.exe hello.exe

rm ./main/lfs/*.elf
python "$IDF_PATH/components/spiffs/spiffsgen.py" 0x420000 ./main/lfs ./main/spiffs.bin \
	--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
	--no-magic --no-magic-len
esptool_version=$(esptool.py version | tail -n 1)
esptool_major=${esptool_version%%.*}
if [[ ! "${esptool_major}" =~ ^[0-9]+$ ]]; then
	printf 'Could not determine esptool version from: %s\n' "${esptool_version}" >&2
	exit 1
fi
if (( esptool_major >= 5 )); then
	esptool_flash_command=write-flash
else
	esptool_flash_command=write_flash
fi
esptool.py --chip auto --port "${PORT}" "${esptool_flash_command}" 0x3D0000 ./main/spiffs.bin
