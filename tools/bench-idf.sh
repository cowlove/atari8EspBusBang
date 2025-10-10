#!/bin/bash -ex 
cd `dirname $0`/..
. ./tools/config

if [ "$1" == "-u" ]; then
    shift;
    UPDATE_REF=true
else 
    UPDATE_REF=false
fi

PTEST="$1"
TEST_SEC="$2"
if [ "$PTEST" == "" ]; then PTEST=1; fi
if [ "$TEST_SEC" == "" ]; then TEST_SEC=10; fi

mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF 

./tools/updateGitH.sh
idf.py --ccache -DPROFILEMODE=${PTEST} -DTEST_SEC=${TEST_SEC} -p ${PORT} app flash
make -C main PORT=${PORT} cat | cat_until "DONE" ${PORT} |  tee out/cat.out 

touch out/timing${PTEST}.txt
grep HIST out/cat.out  > out/timing${PTEST}.txt
if $UPDATE_REF; then cp out/timing${PTEST}.txt out/timing${PTEST}.last.txt; fi 

./tools/compareTimings.sh ./out/timing${PTEST}.last.txt ./out/timing${PTEST}.txt 
