#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DRY_RUN=false
for arg in "$@"; do
	case "$arg" in
		--dry-run) DRY_RUN=true ;;
		*) printf 'Unknown option: %s\nUsage: %s [--dry-run]\n' "$arg" "$0" >&2; exit 2 ;;
	esac
done

SWEEP_REPS="${SWEEP_REPS:-10}"
if [[ ! "$SWEEP_REPS" =~ ^[1-9][0-9]*$ ]]; then
	printf 'SWEEP_REPS must be a positive integer (got %q)\n' "$SWEEP_REPS" >&2
	exit 2
fi

SWEEP_INFO="$(python3 - "$ROOT" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
pattern = re.compile(
    r"(?P<comment>/\*\s*SWEEP\s+(?P<start>-?\d+)\s+(?P<step>-?\d+)\s+(?P<end>-?\d+)\s*\*/)(?P<gap>\s*)(?P<value>-?\d+)"
)
ignored = {".git", "build", "out", "stash", "managed_components"}
matches = []
for path in root.rglob("*"):
    if not path.is_file() or path.suffix not in {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}:
        continue
    if ignored.intersection(path.relative_to(root).parts):
        continue
    text = path.read_bytes().decode("utf-8", errors="ignore")
    for match in pattern.finditer(text):
        matches.append((path, match))

if len(matches) != 1:
    locations = ", ".join(f"{path.relative_to(root)}" for path, _ in matches) or "none"
    print(f"Expected exactly one /*SWEEP start step end*/integer annotation; found {len(matches)} ({locations}).", file=sys.stderr)
    sys.exit(2)

path, match = matches[0]
start, step, end = (int(match[name]) for name in ("start", "step", "end"))
if step == 0 or (end - start) * step < 0:
    print(f"Invalid sweep range in {path.relative_to(root)}: start={start}, step={step}, end={end}.", file=sys.stderr)
    sys.exit(2)
print(f"{path.relative_to(root)}\t{start}\t{step}\t{end}")
PY
)"
IFS=$'\t' read -r SWEEP_FILE SWEEP_START SWEEP_STEP SWEEP_END <<< "$SWEEP_INFO"

set_sweep_value() {
	python3 - "$ROOT/$SWEEP_FILE" "$1" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
value = sys.argv[2]
pattern = re.compile(
    r"(?P<comment>/\*\s*SWEEP\s+-?\d+\s+-?\d+\s+-?\d+\s*\*/)(?P<gap>\s*)(?P<value>-?\d+)"
)
text = path.read_text(encoding="utf-8")
matches = list(pattern.finditer(text))
if len(matches) != 1:
    raise SystemExit(f"Expected one sweep annotation in {path}, found {len(matches)}")
text = pattern.sub(lambda match: match["comment"] + match["gap"] + value, text, count=1)
path.write_text(text, encoding="utf-8")
PY
}

printf 'Sweeping %s: %s to %s by %s (%s repetitions/value)\n' \
	"$SWEEP_FILE" "$SWEEP_START" "$SWEEP_END" "$SWEEP_STEP" "$SWEEP_REPS"
mkdir -p ./stash
git diff > ./stash/sweepstart.git

while IFS= read -r sweep_value; do
	if $DRY_RUN; then
		printf 'SWEEP=%s\n' "$sweep_value"
		continue
	fi
	set_sweep_value "$sweep_value"
	printf 'SWEEP=%s\n' "$sweep_value"
	for ((rep = 1; rep <= SWEEP_REPS; ++rep)); do
		./tools/boot.sh -t -DRUNTIME_SEC=660 '-DBOOT_CONFIG="STRESS"'
	done
done < <(seq "$SWEEP_START" "$SWEEP_STEP" "$SWEEP_END")
