#!/bin/bash
# Switch the Atari's Tasmota power plug (MQTT_HOST, TAS in tools/config) and
# wait for the plug to confirm it, retrying the command until it does.
#
# Usage: power_set.sh ON|OFF|STATE
#   ON, OFF  switch, then wait for the plug's report of that state
#   STATE    ask the plug (an empty POWER command changes nothing) and print
#            its state
#
# Tasmota answers every POWER command with stat/<TAS>/RESULT {"POWER":"ON"}
# and stat/<TAS>/POWER ON. Each try subscribes to both, publishes once the
# subscriber has had a moment to connect, and reads replies for up to
# POWER_REPLY_SEC (default 3) seconds. POWER_TRIES (default 5) tries.
#
# Exit codes: 0 confirmed (STATE: answered), 0 also when MQTT_HOST/TAS are
# unset (no power control; nothing done), 1 no confirmation, 2 usage error.
set -u
cd "$(dirname "$0")/.." || exit 2
. ./tools/export_boot_env.sh

want="${1:-}"
case "${want}" in
	ON|OFF) payload="${want}" ;;
	STATE) payload="" ;;
	*) printf 'Usage: %s ON|OFF|STATE\n' "$0" >&2; exit 2 ;;
esac

if [[ -z "${MQTT_HOST:-}" || -z "${TAS:-}" ]]; then
	printf 'MQTT_HOST and TAS are unset; set them in tools/config to enable automatic Atari 8-bit system power cycling in the boot scripts. Skipping power %s.\n' "${want}" >&2
	exit 0
fi

tries="${POWER_TRIES:-5}"
reply_sec="${POWER_REPLY_SEC:-3}"
replies="$(mktemp)"
sub_pid=""
cleanup() {
	[[ -n "${sub_pid}" ]] && kill "${sub_pid}" 2>/dev/null
	rm -f -- "${replies}"
}
trap cleanup EXIT

# The state a reply reports, or nothing: "stat/<TAS>/POWER ON" or
# "stat/<TAS>/RESULT {"POWER":"ON"}" (RESULT also carries other commands'
# answers, which don't count).
reply_state() {
	sed -nE \
		-e "s|^stat/${TAS}/POWER (ON\|OFF)\$|\1|p" \
		-e "s|^stat/${TAS}/RESULT .*\"POWER\":\"(ON\|OFF)\".*|\1|p" \
		"${replies}" | tail -n 1
}

# Sloppily refresh configuration updates
mosquitto_pub -h "${MQTT_HOST}" -t "cmnd/${TAS}/so65" -m "1" # Don't reset config on repeat power cycles
mosquitto_pub -h "${MQTT_HOST}" -t "cmnd/${TAS}/so56" -m "1" # Look for strongest BSSID on startup

for ((try = 1; try <= tries; try++)); do
	: > "${replies}"
	mosquitto_sub -h "${MQTT_HOST}" -v -W "${reply_sec}" \
		-t "stat/${TAS}/RESULT" -t "stat/${TAS}/POWER" > "${replies}" 2>/dev/null &
	sub_pid=$!
	sleep 0.3   # let the subscription reach the broker before the command
	if ! mosquitto_pub -h "${MQTT_HOST}" -t "cmnd/${TAS}/POWER" -m "${payload}"; then
		printf 'power_set: publish to %s failed (try %d of %d)\n' "${MQTT_HOST}" "${try}" "${tries}" >&2
	fi
	got=""
	deadline=$((SECONDS + reply_sec + 1))
	while ((SECONDS < deadline)); do
		got="$(reply_state)"
		[[ -n "${got}" ]] && break
		kill -0 "${sub_pid}" 2>/dev/null || break
		sleep 0.1
	done
	[[ -z "${got}" ]] && got="$(reply_state)"
	kill "${sub_pid}" 2>/dev/null
	wait "${sub_pid}" 2>/dev/null
	sub_pid=""
	if [[ -n "${got}" ]] && [[ "${want}" == STATE || "${got}" == "${want}" ]]; then
		if [[ "${want}" == STATE ]]; then
			printf 'POWER STATE %s\n' "${got}"
		else
			printf 'POWER %s\n' "${want}"
		fi
		((try > 1)) && printf 'power_set: confirmed on try %d of %d\n' "${try}" "${tries}" >&2
		exit 0
	fi
	printf 'power_set: no %s confirmation from %s (try %d of %d)%s\n' "${want}" "${TAS}" "${try}" "${tries}" \
		"${got:+; it reports ${got}}" >&2
done
printf 'power_set: POWER %s NOT CONFIRMED by %s after %d tries\n' "${want}" "${TAS}" "${tries}" >&2
exit 1
