#!/bin/bash
# Ask a running main to stop ("exit 2": it ends the run as a user-requested
# halt; boot.sh's capture then sees DONE). Through main's own USB console
# (PORT) when it's there, and through the Sidecar's console (SIDECAR_PORT)
# when one is configured: its "main <line>" passes the line to main, which
# fetches it with its next status line (~2 s), so this also reaches a main
# whose USB console is gone (e.g. .6: GPIO19 is the link). Writing to the
# Sidecar's port works while boot.sh -sc's cat reads it.
cd `dirname $0`/..
. ./tools/export_boot_env.sh

sent=0
if [[ -n "${PORT:-}" && -c "${PORT}" ]]; then
	echo "exit 2" > "${PORT}" && sent=1
fi
if [[ -n "${SIDECAR_PORT:-}" && -c "${SIDECAR_PORT}" ]]; then
	printf 'main exit 2\n' > "${SIDECAR_PORT}" && sent=1
fi
if ((!sent)); then
	printf 'halt.sh: neither PORT (%s) nor SIDECAR_PORT (%s) is a serial device\n' "${PORT:-}" "${SIDECAR_PORT:-}" >&2
	exit 2
fi
