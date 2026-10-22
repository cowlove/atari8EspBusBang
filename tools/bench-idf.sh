#!/bin/bash -ex 
cd `dirname $0`/..
. ./tools/export_build_env.sh
. ./tools/export_boot_env.sh

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

./tools/updateGitH.sh
idf.py --ccache -DBOOT_CONFIG=\"BENCH\" -DPROFILEMODE=${PTEST} -DTEST_SEC=${TEST_SEC} -p ${PORT} app flash
make -C main PORT=${PORT} 1cat | ./tools/cat_until.sh -t $(( $TEST_SEC + 30 )) "DONE" ${PORT} |  tee out/cat.out
GIT="$(git describe --abbrev=6 --dirty --always)"
BRANCH="$(git branch --show-current)"
BRANCH_TAG="${BRANCH//\//_}"

BASE=./out/timing${PTEST}
git diff HEAD > ${BASE}.git_diff
GIT="$(git describe --abbrev=6 --dirty --always)"
FNAME=${BASE}.${BRANCH_TAG}.${GIT}.$(md5sum ${BASE}.git_diff | cut -c 1-6)
git diff HEAD > ${FNAME}.diff

touch ${FNAME}.txt
grep HIST ./out/cat.out  > ${FNAME}.txt
BASELINE=${BASE}.${BRANCH_TAG}.baseline.txt
if $UPDATE_REF; then cp ${FNAME}.txt ${BASELINE}; fi
./tools/compareTimings.sh ${BASELINE} ${FNAME}.txt
