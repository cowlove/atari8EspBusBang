#!/bin/bash
# The bench pair's update soak over the network: back-to-back
# `boot.sh -net <sidecar host>` FAKE_SIO runs, each with new BOOT_TOKENs so
# every run really updates: main by OTA over the link, then the Sidecar
# itself (its spare OTA slot, a restart, the rollback probation), then the
# run. One summary line per run: the result, what was updated, the
# FAKE_SIO verdict and the run's capture in stash/.
#
#   tools/bench_net_soak.sh [-H sidecar_host] [-t run_sec] [-n runs]
#                           [-m both|main|sidecar] [-s fake_sio_seed] [-o summary]
#
# -H defaults to $SIDECAR_HOST (tools/config). -t: main's run time (default
# 180 s). -m: which app gets a new token each run (default both; the other
# keeps the token of the soak's start, so after the first run it isn't
# updated again). Tokens are the run's start time (date +%s): never one
# already installed, even across soaks. Stops after -n runs (default 1000)
# or when stash/bench_net_soak.STOP exists (checked between runs).
# Nothing over USB: the Sidecar needs the 16 MB layout already (one
# boot.sh -sc). Bench only: boot.sh refuses FAKE_* builds where
# tools/config powers an Atari.
set -u
cd "$(dirname "$0")/.."
. tools/export_build_env.sh > /dev/null 2>&1
. tools/export_boot_env.sh
HOST="${SIDECAR_HOST:-}"
RUN_SEC=180
RUNS=1000
MODE=both
SEED=1
SUMMARY=stash/bench_net_soak.summary
while getopts "H:t:n:m:s:o:" opt; do
    case $opt in
        H) HOST=$OPTARG ;;
        t) RUN_SEC=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        m) MODE=$OPTARG ;;
        s) SEED=$OPTARG ;;
        o) SUMMARY=$OPTARG ;;
        *) exit 2 ;;
    esac
done
case "$MODE" in both|main|sidecar) ;; *) echo "-m both|main|sidecar" >&2; exit 2 ;; esac
if [[ -z "$HOST" ]]; then
    echo "bench_net_soak: no Sidecar host (-H, or SIDECAR_HOST in tools/config)" >&2
    exit 2
fi
STOP=stash/bench_net_soak.STOP
LAST=stash/bench_net_soak.last
rm -f "$STOP"
mkdir -p stash
ARGS=(-net "$HOST" -t "$RUN_SEC" -DFAKE_CLOCK=1 -DFAKE_SIO=1 -DFAKE_SIO_SEED="$SEED" -DBOOT_CONFIG=BENCH)
start_tok=$(date +%s)
echo "# bench net soak $(git describe --always --dirty) $(date): $RUN_SEC s runs, -m $MODE, ${ARGS[*]}" >> "$SUMMARY"

for ((i = 0; i < RUNS; ++i)); do
    [[ -e "$STOP" ]] && break
    tok=$(date +%s)
    main_tok=$start_tok
    sidecar_tok=$start_tok
    [[ $MODE != sidecar ]] && main_tok=$tok
    [[ $MODE != main ]] && sidecar_tok=$tok
    t0=$(date +%s)
    SIDECAR_DEFINES="-DBOOT_TOKEN=$sidecar_tok" \
        timeout $((RUN_SEC + 1500)) ./tools/boot.sh "${ARGS[@]}" -DBOOT_TOKEN="$main_tok" > "$LAST" 2>&1
    rc=$?
    log=$(ls -t stash/*.mainapp.output 2> /dev/null | head -1)
    ota=$(grep -ac 'updating main by OTA' "$LAST")
    restart=$(grep -ac 'restarted into its new app' "$LAST")
    pass=$(grep -ac "FAKE_SIO PASS" "$LAST")
    sio=$(grep -a " io: " "$LAST" | tail -1 | grep -o "sio [0-9]* fail [0-9]* locks [0-9]*")
    bad=""
    [[ $rc != 0 ]] && bad+=" rc$rc"
    [[ $pass == 0 ]] && bad+=" no-PASS"
    grep -aq "FAKE_SIO FAIL" "$LAST" && bad+=" FAKE_SIO-FAIL"
    [[ $MODE != sidecar && $ota == 0 ]] && bad+=" no-main-OTA"
    [[ $MODE != main && $restart == 0 ]] && bad+=" no-Sidecar-update"
    grep -aq '^UPDATE_FAILED\|MAIN_UPDATE_FAILED\|MAIN_UPDATE_STALLED' "$LAST" && bad+=" update-failed"
    grep -aq "Backtrace\|Guru Meditation\|COREFILE_AVAILABLE" "$LAST" && bad+=" crash"
    # Keep an anomaly's whole boot.sh output (the capture in stash/ has the console only).
    [[ -n $bad ]] && cp "$LAST" "stash/bench_net_soak.run$i.$tok.log"
    echo "$(date +%H:%M:%S) run $i $( [[ -n $bad ]] && echo "ANOMALY$bad" || echo ok ) $(( $(date +%s) - t0 ))s" \
         "| main $main_tok ($ota OTA) Sidecar $sidecar_tok ($restart restart) | $sio | $(basename "${log:-none}")" >> "$SUMMARY"
done
echo "# bench net soak finished $(date)" >> "$SUMMARY"
