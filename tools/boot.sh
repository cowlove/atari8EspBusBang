#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/idf_env.sh
. ./tools/config

PORT="${PORT:-${ESPPORT:-}}"
if [[ -z "${PORT}" ]]; then
	printf 'PORT is unset. Set it in tools/config or export PORT=/dev/ttyACM0 before running this script.\n' >&2
	exit 2
fi
if [[ ! -c "${PORT}" ]]; then
	printf 'PORT is not a character device: %s\n' "${PORT}" >&2
	exit 2
fi

QUICK=false
if [ "$1" == "-q" ]; then
	shift
	QUICK=true
fi

if [ "$(ls -1tr main/lfs/* main/spiffs.bin | tail -1)" != "main/spiffs.bin" ]; then
	echo Rebuilding and flashing spiffs
	./tools/spiffs_flash.sh
fi

$QUICK || rm -rf ./build

./tools/updateGitH.sh
TAG=`date +%Y%m%d.%H%M%S`
HW_TYPE=`pwd | sed -E 's/[^.]*//' | tr -d .`
IDF_ARGS=("$@")
if [ -n "${HW_TYPE}" ]; then
	IDF_ARGS+=("-DHW_TYPE=\"${HW_TYPE}\"")
fi


mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF 
# Use the idf.py flash command below; it selects the esptool.py installed with this IDF.

git diff > ./stash/${TAG}.git_diff
GIT="$(git describe --abbrev=6 --dirty --always)"
echo ${GIT} >> ./stash/${TAG}.git_version
git diff > ./stash/${TAG}.${GIT}.$(md5sum ./stash/${TAG}.git_diff | cut -c 1-6)
if $QUICK; then
	( cd ./build && ESPPORT="${PORT}" ninja app-flash )
else
	idf.py "${IDF_ARGS[@]}" --ccache build flash -p "${PORT}"
fi 
#touch start.ts
( sleep 4 && mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON ) &
(while sleep .1; do if [[ -c "${PORT}" ]]; then stty -F "${PORT}" -echo raw; cat "${PORT}"; fi; done) \
    | ./tools/cat_until.sh -t 200000 DONE "${PORT}" | tee ./stash/${TAG}.output
