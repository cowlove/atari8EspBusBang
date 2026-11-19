#!/bin/bash
# The bench pair's soak, for many link initial conditions: one boot.sh -sc
# FAKE_SIO build and flash, then back-to-back `boot.sh -sc -qq` runs, each
# resetting the Sidecar (USB) and through it main (EN): both boards boot
# afresh, so new phases, ring positions and a new start for slot tuning,
# with nothing rebuilt or reflashed. One summary line per run, from the
# run's log (boot.sh keeps it in stash/ with the git state): the result,
# the SIO counts, main's lock count, the Sidecar's "startup:" line and
# main's first lock.
#
#   tools/bench_soak.sh [-t run_sec] [-n runs] [-s fake_sio_seed] [-L] [-o summary]
#
# -L adds -DLINK_TEST=1 (the boot-time link measurements: ~10 s of each
# run). Needs a Sidecar build of main (sdkconfig: CONFIG_A8BB_SIDECAR_ENABLE,
# as every -sc run). Stops after -n runs (default 1000) or when
# stash/bench_soak.STOP exists (checked between runs). Bench only: boot.sh
# refuses FAKE_* builds where tools/config powers an Atari.
set -u
cd "$(dirname "$0")/.."
RUN_SEC=30
RUNS=1000
SEED=1
LINK_TEST=()
SUMMARY=stash/bench_soak.summary
while getopts "t:n:s:Lo:" opt; do
    case $opt in
        t) RUN_SEC=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        s) SEED=$OPTARG ;;
        L) LINK_TEST=(-DLINK_TEST=1) ;;
        o) SUMMARY=$OPTARG ;;
        *) exit 2 ;;
    esac
done
. tools/export_build_env.sh > /dev/null 2>&1
. tools/export_boot_env.sh
STOP=stash/bench_soak.STOP
rm -f "$STOP"
mkdir -p stash
ARGS=(-t "$RUN_SEC" -DFAKE_CLOCK=1 -DFAKE_SIO=1 -DFAKE_SIO_SEED="$SEED" -DBOOT_CONFIG=\"BENCH\" "${LINK_TEST[@]}")
echo "# bench soak $(git describe --always --dirty) $(date): $RUN_SEC s runs, ${ARGS[*]}" >> "$SUMMARY"

# Monitors a run can leave behind (AGENTS.md): cat_until and its cat on the
# Sidecar's port. By process name and by the port, never by pattern.
clear_monitors() {
    pkill -x cat_until.sh 2> /dev/null
    [[ -n "${SIDECAR_PORT:-}" ]] && fuser -k -TERM "$SIDECAR_PORT" > /dev/null 2>&1
    sleep 1
}

for ((i = 0; i < RUNS; ++i)); do
    [[ -e "$STOP" ]] && break
    clear_monitors
    # The first run builds and flashes (and OTAs main); the rest only reset.
    quick=()
    ((i > 0)) && quick=(-qq)
    t0=$(date +%s)
    timeout $((RUN_SEC + 1500)) ./tools/boot.sh -sc "${quick[@]}" "${ARGS[@]}" > stash/bench_soak.last 2>&1
    rc=$?
    log=$(ls -t stash/*.mainapp.output 2> /dev/null | head -1)
    pass=$(grep -a -c "FAKE_SIO PASS" "$log")
    failed=$(grep -a -c "FAKE_SIO FAIL" "$log")
    io=$(grep -a " io: " "$log" | tail -1)
    sio=$(grep -o "sio [0-9]* fail [0-9]* locks [0-9]*" <<< "$io")
    startup=$(grep -a "Sidecar: startup:" "$log" | head -1 | sed 's/.*startup: //')
    mainlock=$(grep -a "Sidecar: link \(locked\|UNLOCKED\) at" "$log" | head -1 | sed 's/.*Sidecar: link //')
    nop=$(grep -a "Sidecar round trip Nop: " "$log" | tail -1 | grep -o "us min.*")
    bad=""
    [[ $rc != 0 ]] && bad+=" rc$rc"
    [[ $pass == 0 ]] && bad+=" no-PASS"
    [[ $failed != 0 ]] && bad+=" FAKE_SIO-FAIL"
    grep -aq "Backtrace\|Guru Meditation\|COREFILE_AVAILABLE" "$log" stash/bench_soak.last && bad+=" crash"
    echo "$(date +%H:%M:%S) run $i $( [[ -n $bad ]] && echo "ANOMALY$bad" || echo ok ) $(( $(date +%s) - t0 ))s" \
         "| $sio ${nop:+| nop $nop }| main $mainlock | startup $startup | $(basename "$log")" >> "$SUMMARY"
done
clear_monitors
echo "# bench soak finished $(date)" >> "$SUMMARY"
