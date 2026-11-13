#!/bin/bash
# Autoroute a KiCad board with Freerouting, headless, then DRC it.
#
#   hardware/tools/autoroute.sh <board.kicad_pcb> [out.kicad_pcb] [passes]
#
# Exports Specctra DSN (pcbnew) without the GND net and with the routing
# boundary inset 0.5 mm (KiCad's copper-to-edge clearance, which Freerouting
# ignores), routes it with Freerouting, imports the SES, pours GND on both
# layers,
# all into a copy of the board (out.kicad_pcb, default <board>-routed.kicad_pcb;
# the input is never touched), and runs kicad-cli DRC on the result. The work
# files (.dsn, .ses, freerouting log, DRC report) go next to the output.
#
# Needs: KiCad 9 or 10 with its Python module for /usr/bin/python3 (KiCad PPA),
# Freerouting (FREEROUTING_JAR, default ~/opt/freerouting/freerouting.jar)
# and a Java new enough for it (JAVA, default ~/opt/jre25/bin/java, else
# java on PATH).
set -euo pipefail

board="${1:?usage: $0 <board.kicad_pcb> [out.kicad_pcb] [passes]}"
out="${2:-${board%.kicad_pcb}-routed.kicad_pcb}"
passes="${3:-30}"
jar="${FREEROUTING_JAR:-$HOME/opt/freerouting/freerouting.jar}"
java="${JAVA:-$HOME/opt/jre25/bin/java}"
[[ -x "$java" ]] || java=java
here="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$(dirname "$out")"
stem="${out%.kicad_pcb}"
dsn="$stem.dsn"
ses="$stem.ses"
log="$stem.freerouting.log"
drc="$stem.drc.rpt"

# The project's rules (.kicad_pro net classes, .kicad_dru) go along, so DRC
# of the copy matches DRC of the board.
for ext in kicad_pro kicad_dru; do
    [[ -f "${board%.kicad_pcb}.$ext" ]] && cp "${board%.kicad_pcb}.$ext" "$stem.$ext"
done
/usr/bin/python3 "$here/specctra.py" export "$board" "$dsn" --inset-mm 0.5 \
    $(for n in ${SKIP_NETS-}; do echo --skip-net "$n"; done)
echo "routing $(basename "$dsn") (up to $passes passes; log: $log)"
start=$(date +%s)
# Freerouting writes the session only when it finishes, so the pass limit is
# what bounds a run (each pass takes ~20 s on this board); the wall-clock
# timeout (ROUTE_TIMEOUT seconds) is a backstop, and a run it stops yields
# only the log ("N unrouted" per pass). The optimizer stage is off unless
# OPTIMIZE=true: on sidecar_pcb_xl it took 90 s a pass and improved nothing.
timeout "${ROUTE_TIMEOUT:-1800}" "$java" -jar "$jar" -de "$dsn" -do "$ses" -mp "$passes" \
    --gui.enabled=false --router.optimizer.enabled="${OPTIMIZE:-false}" >"$log" 2>&1 || {
    echo "freerouting failed or timed out (ROUTE_TIMEOUT ${ROUTE_TIMEOUT:-1800} s):" >&2
    grep -o "pass #[0-9]* .*unrouted[^)]*" "$log" | tail -3 >&2 || tail -20 "$log" >&2
    exit 1
}
echo "routed in $(( $(date +%s) - start )) s"
[[ -s "$ses" ]] || { echo "no session file written" >&2; tail -20 "$log" >&2; exit 1; }
/usr/bin/python3 "$here/specctra.py" import "$board" "$ses" "$out" --pour GND
# Freerouting sometimes leaves a GND track too close to another net: drop
# those (the pour covers GND), then the real DRC.
kicad-cli pcb drc --format json --severity-all -o "$stem.drc.json" "$out" >/dev/null
/usr/bin/python3 "$here/specctra.py" cleanup "$out" "$stem.drc.json" --net GND
kicad-cli pcb drc --severity-all -o "$drc" "$out" | grep -i "found" || true
grep "^\[" "$drc" | sed 's/\].*/]/' | sort | uniq -c || true
echo "routed board: $out"
