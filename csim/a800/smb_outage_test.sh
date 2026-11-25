#!/bin/bash
# The SMB stitch's recovery (components/disk_image/diskStitchSmb.h), in csim:
# BOOT_CONFIG=SDX_CSIM_SMB puts D4 (X.BAT's SMB copies of FOO) on an SMB
# stitch on the Sidecar, through tcp_proxy.py on 127.0.0.1:4450 to the share
# smb://$SMB_HOST/pub, in pub/csim-smbtest (which must exist there, holding
# a FOO: any file of a few KB). Two outages while the workload runs:
#   freeze  the proxy stopped (SIGSTOP) for FREEZE_SEC: requests time out,
#           as on the test pool (SMB2_STATUS_IO_TIMEOUT)
#   drop    the proxy killed for DROP_SEC: connections reset, new ones
#           refused (the backoff), then it comes back
# Pass: after each outage the Sidecar logs "SMB: ... reconnected" and D4's
# I/O works again (no SMB errors in the last SETTLE_SEC), and the run ends
# at its time with no SIO failures.
#
#   csim/a800/smb_outage_test.sh [out_dir]
#
# Wall-clock phases (the share is real, so its timeouts are wall time).
# Progress: csim has no screen lines (the Sidecar's requests skip the
# scrape), so each phase prints FOO2/FOO3's mtimes on the share, read over
# ssh from SMB_SSH:SMB_DIR (the share's directory): they move while the
# copies work.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/tmp/smb_outage_test}
SMB_HOST=${SMB_HOST:-miner6.local}
SMB_SSH=${SMB_SSH:-$SMB_HOST} SMB_DIR=${SMB_DIR:-/home/jim/tmp/smb/csim-smbtest}
WARM_SEC=${WARM_SEC:-60} FREEZE_SEC=${FREEZE_SEC:-20} DROP_SEC=${DROP_SEC:-40} SETTLE_SEC=${SETTLE_SEC:-60}
mkdir -p "$OUT"
LOG=$OUT/csim.log
HOST_IP=$(python3 -c "import socket,sys; print(socket.gethostbyname(sys.argv[1]))" "$SMB_HOST")

proxy() { python3 "$HERE/tcp_proxy.py" 4450 "$HOST_IP" 445 & PROXY=$!; }
stamp() { echo "$(date +%H:%M:%S) $*  [share: $(ssh -o ConnectTimeout=10 "$SMB_SSH" "cd $SMB_DIR && stat -c '%n %y' FOO2 FOO3 2>/dev/null | cut -c1-24 | tr '\n' ' '")]" | tee -a "$OUT/events.txt"; }
cleanup() { kill -CONT $PROXY 2>/dev/null; kill $PROXY 2>/dev/null; [ -n "${CSIM:-}" ] && kill $CSIM 2>/dev/null; }
trap cleanup EXIT
: > "$OUT/events.txt"

proxy
stamp "proxy up (pid $PROXY) -> $HOST_IP:445"
BOOT_CONFIG=SDX_CSIM_SMB RUN_SEC=${RUN_SEC:-100000} "$HERE/run.sh" > "$LOG" 2>&1 &
CSIM=$!
stamp "csim started (pid $CSIM)"
# Wait for the build and the boot: D4 mounted.
ssh "$SMB_SSH" "rm -f $SMB_DIR/FOO2 $SMB_DIR/FOO3"
for i in $(seq 900); do grep -aq "D4: .smb.*mounted" "$LOG" 2>/dev/null && break; sleep 1; done
sleep "$WARM_SEC"
stamp "freeze: proxy stopped for $FREEZE_SEC s"
kill -STOP $PROXY; sleep "$FREEZE_SEC"; kill -CONT $PROXY
stamp "freeze over"
sleep "$SETTLE_SEC"
stamp "drop: proxy killed for $DROP_SEC s"
kill $PROXY; wait $PROXY 2>/dev/null; sleep "$DROP_SEC"
proxy
stamp "drop over: proxy up again (pid $PROXY)"
sleep "$SETTLE_SEC"
stamp "stopping csim"
kill $CSIM 2>/dev/null; wait $CSIM 2>/dev/null; CSIM=

echo "--- SMB lines:"
grep -anE "SMB:|smb2_|readdir failed|Failed to (read|write)" "$LOG" | cut -c1-160 | head -40
echo "--- D4 failures: $(grep -ac 'D4: .*failed' "$LOG")"
echo "--- reconnects: $(grep -ac 'SMB: .*reconnected' "$LOG"), failed reconnects: $(grep -ac 'reconnecting to .* failed' "$LOG")"
echo "--- last io lines:"
grep -a " io: " "$LOG" | tail -3 | cut -c1-140
