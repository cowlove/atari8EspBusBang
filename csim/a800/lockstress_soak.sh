#!/bin/bash
# Parallel csim link soak: many csim runs at once, each with its own seed
# (CSIM_SEED), running the Sidecar's lockstress (repeated frame-lock
# breaks) beside the normal SDX disk workload. One summary line per run;
# the full log is kept only for a run that looks wrong (any timeout, false
# lock, SIO failure, missing DONE, or an exit other than the run time).
#
#   csim/a800/lockstress_soak.sh [-j jobs] [-s lockstress_sec] [-n runs]
#                                [-m "read peek hint"] [-b binary] [-o dir]
#
# The binary is copied first (-b, default csim/a800/build-atari/
# atari800-csim, built for BOOT_CONFIG=SDX_CSIM), so rebuilding while the
# soak runs is safe. Runs until -n runs are done (default: until stopped;
# touch <dir>/STOP to finish the runs in flight and stop).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
JOBS=10
LS_SEC=3600
RUNS=0
MODES="read peek hint"
SRC_BIN=$HERE/build-atari/atari800-csim
OUT=$HERE/soak
while getopts "j:s:n:m:b:o:" opt; do
    case $opt in
        j) JOBS=$OPTARG ;;
        s) LS_SEC=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        m) MODES=$OPTARG ;;
        b) SRC_BIN=$OPTARG ;;
        o) OUT=$OPTARG ;;
        *) exit 2 ;;
    esac
done

mkdir -p "$OUT/bin" "$OUT/runs" "$OUT/anomalies"
rm -f "$OUT/STOP"
cp "$SRC_BIN" "$(dirname "$SRC_BIN")/.atari800.cfg" "$OUT/bin/"
# The binary finds libsmb2 at $ORIGIN/../build-libsmb2/lib; the runs find
# the spiffs images at ../../spiffs from their working directories.
ln -sfn "$HERE/build-libsmb2" "$OUT/build-libsmb2"
ln -sfn "$REPO/spiffs" "$OUT/spiffs"
echo "$(git -C "$REPO" describe --always --dirty) $(date)" > "$OUT/bin/REV"
BIN=$OUT/bin/atari800-csim
SUMMARY=$OUT/summary.txt
echo "# soak $(cat "$OUT/bin/REV"): jobs $JOBS, lockstress $LS_SEC s, modes $MODES" >> "$SUMMARY"
read -r -a MODE_LIST <<< "$MODES"

# One run: worker job, run number (seed) n.
run_one() {
    local job=$1 n=$2
    local mode=${MODE_LIST[$((n % ${#MODE_LIST[@]}))]}
    local seed=$n
    local dir=$OUT/runs/j$job   # two below $OUT: ../../spiffs is $OUT/spiffs
    mkdir -p "$dir"
    cp -n "$HERE/SDX450_maxflash1.car" "$dir/" 2>/dev/null
    local log=$dir/run.log
    local t0=$(date +%s)
    ( cd "$dir" && BOOT_CONFIG=SDX_CSIM CSIM_SEED=$seed CSIM_RUN_SEC=$((LS_SEC + 60)) \
        CSIM_SIDECAR_CMD="lockstress $mode $LS_SEC" \
        timeout $((LS_SEC + 600)) "$BIN" -xl -nopatchall ./SDX450_maxflash1.car -turbo -rambo \
        > "$log" 2>&1 )
    local rc=$?
    local done=$(grep -a "lockstress $mode DONE" "$log" | tail -1)
    local io=$(grep -a " io: " "$log" | tail -1)
    local exitr=$(grep -a "^Exit reason" "$log" | tail -1)
    local timeouts=$(sed -n 's/.*timeouts \([0-9]*\).*/\1/p' <<< "$done")
    local falsel=$(sed -n 's/.*FALSE LOCKS \([0-9]*\).*/\1/p' <<< "$done")
    local fail=$(sed -n 's/.* fail \([0-9]*\).*/\1/p' <<< "$io")
    local bad=""
    [ -z "$done" ] && bad+=" no-DONE"
    [ "${timeouts:-0}" != 0 ] && bad+=" timeouts"
    [ "${falsel:-0}" != 0 ] && bad+=" false-locks"
    [ "${fail:-0}" != 0 ] && bad+=" sio-fail"
    [[ "$exitr" != *"Specified run time reached"* ]] && bad+=" exit($rc)"
    local line="seed $seed $mode rc $rc $(( $(date +%s) - t0 ))s |${done#*DONE:} | sio fail ${fail:-?}"
    if [ -n "$bad" ]; then
        cp "$log" "$OUT/anomalies/seed$seed-$mode.log"
        line="ANOMALY$bad: $line | $exitr"
    fi
    echo "$line" >> "$SUMMARY"
}

worker() {
    local job=$1 n=$job
    while [ ! -e "$OUT/STOP" ]; do
        if [ "$RUNS" -gt 0 ] && [ "$n" -ge "$RUNS" ]; then
            break
        fi
        run_one "$job" "$n"
        n=$((n + JOBS))
    done
}

for ((job = 0; job < JOBS; job++)); do
    worker "$job" &
done
wait
echo "# soak finished $(date)" >> "$SUMMARY"
