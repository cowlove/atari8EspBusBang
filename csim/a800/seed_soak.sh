#!/bin/bash
# Many short csim runs with randomized initial conditions: each run's
# CSIM_SEED (its number) sets where every I2S channel starts (the link's bit
# phase, byte lane and ring position) and the run's TX FIFO lag
# (CSIM_I2S_TX_LAG_BYTES, 150-220 around the bench's ~184), cycling through
# the workloads:
#   sdx          SDX boot, main fetching the Test object (CSIM_LINK_TEST=1)
#   read peek hint align   the Sidecar's lockstress, beside the SDX boot
#   fakesio      FAKE_SIO (core 0 plays the 6502's PBI side, idle cart)
#   fakesio_bus  FAKE_SIO in bus-cycle mode (banks, $D301)
# One summary line per run, with the link's startup conditions (the
# Sidecar's "startup:" line, main's first lock); the full log is kept only
# for an anomaly: no DONE, a FAKE_SIO FAIL or no PASS, SIO failures,
# lockstress timeouts or false locks, an exit other than the run time.
#
#   csim/a800/seed_soak.sh [-j jobs] [-s sec] [-n runs] [-m "modes"] [-o dir]
#
# Builds its three binaries first (into the usual build dir, then copied),
# then leaves the default build behind. Runs until -n runs (default: until
# stopped; touch <dir>/STOP to finish the runs in flight and stop).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
JOBS=8
SEC=30
RUNS=0
MODES="sdx read peek hint align fakesio fakesio_bus"
OUT=$HERE/seed_soak   # ignored by git (.gitignore)
while getopts "j:s:n:m:o:" opt; do
    case $opt in
        j) JOBS=$OPTARG ;;
        s) SEC=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        m) MODES=$OPTARG ;;
        o) OUT=$OPTARG ;;
        *) exit 2 ;;
    esac
done

mkdir -p "$OUT/bin" "$OUT/runs" "$OUT/anomalies"
rm -f "$OUT/STOP"
build() {   # name, make args...
    local name=$1
    shift
    make -C "$HERE" "$@" > "$OUT/build-$name.log" 2>&1 || { echo "build $name failed: $OUT/build-$name.log"; exit 1; }
    cp "$HERE/build-atari/atari800-csim" "$OUT/bin/atari800-$name"
}
build fakesio FAKE_SIO=1 BUS_CYCLE=0 BOOT_CONFIG=FAKESIO_CSIM hybrid
build fakesio_bus FAKE_SIO=1 BUS_CYCLE=1 BOOT_CONFIG=FAKESIO_CSIM_BUS hybrid
build sdx
cp "$HERE/build-atari/.atari800.cfg" "$OUT/bin/"
( cd "$HERE" && ./make_idle_cart.py idle.car > /dev/null && ./make_d1.sh > /dev/null )
# The binaries find libsmb2 at $ORIGIN/../build-libsmb2/lib; the runs find
# the spiffs images at ../../spiffs from their working directories.
ln -sfn "$HERE/build-libsmb2" "$OUT/build-libsmb2"
ln -sfn "$REPO/spiffs" "$OUT/spiffs"
echo "$(git -C "$REPO" describe --always --dirty) $(date)" > "$OUT/bin/REV"
SUMMARY=$OUT/summary.txt
echo "# seed soak $(cat "$OUT/bin/REV"): jobs $JOBS, $SEC s runs, modes $MODES" >> "$SUMMARY"
read -r -a MODE_LIST <<< "$MODES"

run_one() {   # job, run number (the seed)
    local job=$1 n=$2
    local mode=${MODE_LIST[$((n % ${#MODE_LIST[@]}))]}
    local lag=$((150 + (n * 37) % 71))
    local dir=$OUT/runs/j$job   # two below $OUT: ../../spiffs is $OUT/spiffs
    mkdir -p "$dir"
    cp -n "$HERE/SDX450_maxflash1.car" "$HERE/idle.car" "$dir/" 2>/dev/null
    local log=$dir/run.log
    local bin config args env=()
    case $mode in
        sdx) bin=sdx; config=SDX_CSIM; args=(-xl -nopatchall ./SDX450_maxflash1.car -turbo -rambo); env=(CSIM_LINK_TEST=1) ;;
        read|peek|hint|align) bin=sdx; config=SDX_CSIM; args=(-xl -nopatchall ./SDX450_maxflash1.car -turbo -rambo)
            env=("CSIM_SIDECAR_CMD=lockstress $mode $((SEC > 10 ? SEC - 5 : SEC))") ;;
        fakesio) bin=fakesio; config=FAKESIO_CSIM; args=(-xl -nopatchall ./idle.car -turbo) ;;
        fakesio_bus) bin=fakesio_bus; config=FAKESIO_CSIM_BUS; args=(-xl -nopatchall -turbo) ;;
        *) echo "unknown mode $mode"; return ;;
    esac
    local t0=$(date +%s)
    ( cd "$dir" && env BOOT_CONFIG=$config CSIM_SEED=$n CSIM_I2S_TX_LAG_BYTES=$lag CSIM_RUN_SEC=$SEC CSIM_HTTP_PORT=0 "${env[@]}" \
        timeout $((SEC * 40 + 300)) "$OUT/bin/atari800-$bin" "${args[@]}" > "$log" 2>&1 )
    local rc=$?
    local io=$(grep -a " io: " "$log" | tail -1)
    local exitr=$(grep -a "^Exit reason" "$log" | tail -1)
    local startup=$(grep -a "Sidecar: startup:" "$log" | head -1 | sed 's/.*startup: //')
    local mainlock=$(grep -a "Sidecar: link \(locked\|UNLOCKED\) at" "$log" | head -1 | sed 's/.*Sidecar: link //')
    local fail=$(sed -n 's/.* sio [0-9]* fail \([0-9]*\).*/\1/p' <<< "$io")
    local rt=$(grep -o "rt [0-9]*/[0-9]* us" <<< "$io")
    local bad="" extra=""
    grep -aqE "^(\[[0-9]{5}\] )?DONE" "$log" || bad+=" no-DONE"
    [ "${fail:-0}" != 0 ] && bad+=" sio-fail"
    [[ "$exitr" != *"Specified run time reached"* ]] && bad+=" exit($rc)"
    case $mode in
        fakesio|fakesio_bus)
            grep -aq "FAKE_SIO FAIL" "$log" && bad+=" fakesio-FAIL"
            grep -aq "FAKE_SIO PASS" "$log" || bad+=" no-PASS" ;;
        read|peek|hint|align)
            local done=$(grep -a "lockstress $mode DONE" "$log" | tail -1)
            [ -z "$done" ] && bad+=" no-lockstress-DONE"
            local to=$(sed -n 's/.*timeouts \([0-9]*\).*/\1/p' <<< "$done")
            local fl=$(sed -n 's/.*FALSE LOCKS \([0-9]*\).*/\1/p' <<< "$done")
            [ "${to:-0}" != 0 ] && bad+=" timeouts"
            [ "${fl:-0}" != 0 ] && bad+=" false-locks"
            extra=" | breaks $(sed -n 's/.*breaks \([0-9]*\).*/\1/p' <<< "$done")" ;;
    esac
    local made=$(sed -n 's/.*i2slink sidecar: moves announced [0-9]* made \([0-9]*\).*/\1/p' "$log")
    local followed=$(sed -n 's/.*i2slink host: moves.*followed \([0-9]*\).*/\1/p' "$log")
    # Clean runs: a short line. Anomalies: everything, and the log.
    local line="seed $n $mode lag ${lag}B ok ${rt:-}${extra} moves ${made:-?}/${followed:-?}"
    if [ -n "$bad" ]; then
        cp "$log" "$OUT/anomalies/seed$n-$mode.log"
        line="ANOMALY$bad: seed $n $mode lag ${lag}B rc $rc $(( $(date +%s) - t0 ))s | sio fail ${fail:-?} ${rt}${extra} | moves made ${made:-?} followed ${followed:-?} | main ${mainlock:-?} | startup ${startup:-?} | $exitr"
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
echo "# seed soak finished $(date)" >> "$SUMMARY"
