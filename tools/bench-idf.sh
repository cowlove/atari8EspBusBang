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
RUN_SEC="${RUN_SEC:-$2}"
if [ "$PROFILE_POINT" == "" ]; then PROFILE_POINT=1; fi
if [ "$RUN_SEC" == "" ]; then RUN_SEC=17; fi

PROFILE_ARGS=(-DPROFILE_MODE=${PROFILE_MODE})
if [ "$PROFILE_MODE" == "PIN" ] || [ "$PROFILE_MODE" == "LEGACY" ]; then
    PROFILE_ARGS+=(-DPROFILE_POINT=${PROFILE_POINT})
fi

# Build in build-bench/, with its own sdkconfig: the project's sdkconfig
# plus the overrides in sdkconfig.bench. A Kconfig change rebuilds all of
# IDF, so keeping it apart from build/ means each build stays incremental.
# The bench's cached -D options (BOOT_CONFIG, FAKE_CLOCK, PROFILE_*,
# RUN_SEC) also stay out of build/. IDF reads SDKCONFIG_DEFAULTS only when
# it creates the sdkconfig, so regenerate it when either source changes.
BENCH_BUILD="$PWD/build-bench"
BENCH_SDKCONFIG="$BENCH_BUILD/sdkconfig"
BENCH_SRC_SUM="$(cat sdkconfig sdkconfig.bench | md5sum | cut -d' ' -f1)"
if [ "$(cat "$BENCH_BUILD/sdkconfig.src.md5" 2>/dev/null)" != "$BENCH_SRC_SUM" ]; then
    rm -f "$BENCH_SDKCONFIG"
fi
mkdir -p "$BENCH_BUILD"

./tools/updateGitH.sh
idf.py --ccache -B "$BENCH_BUILD" -DSDKCONFIG="$BENCH_SDKCONFIG" \
    -DSDKCONFIG_DEFAULTS="$PWD/sdkconfig;$PWD/sdkconfig.bench" \
    -DBOOT_CONFIG=\"BENCH\" "${PROFILE_ARGS[@]}" -DFAKE_CLOCK=1 -DRUN_SEC=${RUN_SEC} -p ${PORT} app flash
echo "$BENCH_SRC_SUM" > "$BENCH_BUILD/sdkconfig.src.md5"
make -C main PORT=${PORT} 1cat | ./tools/cat_until.sh -t $(( RUN_SEC + 30 )) "DONE" ${PORT} |  tee out/cat.out
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
