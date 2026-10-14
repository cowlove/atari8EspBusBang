#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/idf_env.sh
. ./tools/config

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
HW_ID=`pwd | sed -E 's/[^.]*//' | tr -d .`
IDF_ARGS=("$@")
if [ -n "${HW_ID}" ]; then
	IDF_ARGS+=("-DTEST_HW_ID=${HW_ID}")
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
	( cd ./build &&	ESPPORT=${PORT} ninja app-flash )
else
	idf.py "${IDF_ARGS[@]}" --ccache build flash -p "${PORT}"
fi 
#touch start.ts
( sleep 4 && mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON ) &
(while sleep .1; do if [ -c ${PORT} ]; then stty -F ${PORT} -echo raw; cat ${PORT}; fi; done) \
    | ./tools/cat_until.sh -t 200000 DONE ${PORT} | tee ./stash/${TAG}.output

