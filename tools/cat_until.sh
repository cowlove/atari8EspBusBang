#!/bin/bash
# Read stdin (typically a serial port via a background `cat <port>`) line by
# line until a regex pattern is matched, then exit. Optionally give up early
# if the input stream is idle (no new line) for a given number of seconds.
#
# Usage: cat_until [-t idle_sec] <pattern> [port]
#   <pattern>   ERE matched against each line (like the original script)
#   [port]      optional device path; a matching `cat <port>` process is
#               killed on exit so the serial port is released
#
# Exit codes: 0 pattern matched (or producer EOF), 124 idle timeout,
#             1 usage error.

IDLE_TIMEOUT=0
while getopts ":t:" opt; do
  case $opt in
    t) IDLE_TIMEOUT=$OPTARG ;;
    \?) echo "Usage: $0 [-t idle_sec] <pattern> [port]" >&2; exit 1 ;;
    :) echo "Usage: $0 [-t idle_sec] <pattern> [port] — missing argument to -$OPTARG" >&2; exit 1 ;;
  esac
done
shift $((OPTIND - 1))

PATTERN="$1"
PORT="$2"

if [ -z "$PATTERN" ]; then
  echo "Usage: $0 [-t idle_sec] <pattern> [port]" >&2
  exit 1
fi

# Allow positive fractional seconds (read -t accepts them); reject junk.
if [ "$IDLE_TIMEOUT" != "0" ]; then
  if ! [[ "$IDLE_TIMEOUT" =~ ^[0-9]+([.][0-9]+)?$ ]] || [ "$IDLE_TIMEOUT" = "0.0" ]; then
    echo "$0: -t requires a positive number of seconds, got '$IDLE_TIMEOUT'" >&2
    exit 1
  fi
fi

# Kill only an exact `cat <port>` process; anchored so we cannot match
# unrelated commands, and guarded so an empty port kills nothing.
cleanup() {
  if [ -n "$PORT" ]; then
    pkill -f "^cat ${PORT}$" 2>/dev/null
  fi
}
trap 'cleanup; exit 130' INT TERM
trap 'cleanup' EXIT

read_opts=(-r)
if [ "$IDLE_TIMEOUT" != "0" ]; then
  read_opts+=(-t "$IDLE_TIMEOUT")
fi

LINE_COUNT=0
while :; do
  IFS= read "${read_opts[@]}" line
  rc=$?
  if [ "$rc" -gt 128 ]; then
    # read -t expired with no complete line. Concise single line so summary
    # uses don't wrap; still echoes the pattern for tally consumers.
    echo "timeout after ${IDLE_TIMEOUT} sec, ${LINE_COUNT} lines, no ${PATTERN} seen"
    cleanup
    exit 124
  fi
  if [ "$rc" -ne 0 ]; then
    # Producer closed the stream (EOF).
    cleanup
    exit 0
  fi
  echo "$line"
  LINE_COUNT=$((LINE_COUNT + 1))
  if [[ "$line" =~ $PATTERN ]]; then
    echo "Pattern matched, exiting"
    cleanup
    exit 0
  fi
done
