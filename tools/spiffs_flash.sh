#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/config
. ./tools/idf_env.sh

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

mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
atr ./main/lfs/d1.atr put -l ./main/lfs/x2.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x256.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x192.cmd
atr ./main/lfs/d2.atr put -l ./main/lfs/x.bat

~/opt/llvm-mos/bin/mos-atari8-cart-std-clang ./main/lfs/simplemem.c -Oz -o ./main/lfs/simplemem.rom
~/opt/llvm-mos/bin/mos-atari8-cart-std-clang ./main/lfs/hello.c -Oz -o ./main/lfs/hello.rom
~/opt/llvm-mos/bin/mos-atari8-cart-atarimax128-clang ./main/lfs/stress.c -Oz -o ./main/lfs/stress.rom
python3 ./tools/make_cart.py ./main/lfs/stress.rom ./main/lfs/stress.car
rm ./main/lfs/stress.rom
~/opt/llvm-mos/bin/mos-atari8-dos-clang ./main/lfs/hello.c -Oz -o ./main/lfs/hello.exe
atr ./main/lfs/llvm_d1.atr put ./main/lfs/hello.exe autorun.sys
atr ./main/lfs/d2.atr put ./main/lfs/hello.exe hello.exe

rm ./main/lfs/*.elf
python "$IDF_PATH/components/spiffs/spiffsgen.py" 0x420000 ./main/lfs ./main/spiffs.bin \
	--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
	--no-magic --no-magic-len
esptool.py -c auto -p ${PORT} write_flash 0x3D0000 ./main/spiffs.bin
