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

PROFILE_MODE="${PROFILE_MODE:-PIN}"
PROFILE_POINT="${PROFILE_POINT:-$1}"
TEST_SEC="${TEST_SEC:-$2}"
if [ "$PROFILE_POINT" == "" ]; then PROFILE_POINT=1; fi
if [ "$TEST_SEC" == "" ]; then TEST_SEC=17; fi

PROFILE_ARGS=(-DPROFILE_MODE=${PROFILE_MODE})
if [ "$PROFILE_MODE" == "PIN" ] || [ "$PROFILE_MODE" == "LEGACY" ]; then
    PROFILE_ARGS+=(-DPROFILE_POINT=${PROFILE_POINT})
fi

./tools/updateGitH.sh
idf.py --ccache -DBOOT_CONFIG=\"BENCH\" "${PROFILE_ARGS[@]}" -DFAKE_CLOCK=1 -DTEST_SEC=${TEST_SEC} -p ${PORT} app flash
make -C main PORT=${PORT} 1cat | ./tools/cat_until.sh -t $(( $TEST_SEC + 30 )) "DONE" ${PORT} |  tee out/cat.out
GIT="$(git describe --abbrev=6 --dirty --always)"
BRANCH="$(git branch --show-current)"
BRANCH_TAG="${BRANCH//\//_}"

BASE=./out/timing${PROFILE_MODE}_${PROFILE_POINT}
git diff HEAD > ${BASE}.git_diff
GIT="$(git describe --abbrev=6 --dirty --always)"
FNAME=${BASE}.${BRANCH_TAG}.${GIT}.$(md5sum ${BASE}.git_diff | cut -c 1-6)
git diff HEAD > ${FNAME}.diff

touch ${FNAME}.txt
grep HIST ./out/cat.out  > ${FNAME}.txt
BASELINE=${BASE}.${BRANCH_TAG}.baseline.txt
if $UPDATE_REF; then cp ${FNAME}.txt ${BASELINE}; fi
./tools/compareTimings.sh ${FNAME}.txt ${BASELINE} 
