# Sourced by tools/boot.sh for -sc: build main, and flash and boot it
# through the Sidecar instead of over main's own USB port.
#
#   1. Build main (idf.py build, the same -D args; nothing flashed on PORT).
#   2. Build the Sidecar app and flash it on SIDECAR_PORT (tools/config).
#   3. Stage main's four images in the Sidecar's spiffs
#      (sidecar/flash_spiffs.sh, MAIN_BUILD=./build) and flash that, last,
#      so the Sidecar boots with both.
#   4. Once the Sidecar's console is up, send it "flash": its serial flasher
#      writes main's images from /spiffs/main/, verifies them and resets
#      main into them. It refuses if phi2/D0/D1 are active (a running Atari,
#      or a main board running a FAKE_CLOCK build): boot.sh then fails with
#      what it said. That refusal is a safety check and stays.
#   5. With power control (MQTT_HOST/TAS), the Atari is off for all of that,
#      and on 4 s after the flasher's "done", as boot.sh does after a flash.
#   6. cat_until on the Sidecar's console: main's LOG() output reaches it
#      over the link (SerialOut), so main must be built with
#      CONFIG_A8BB_LOG_VIA_SIDECAR (CONFIG_A8BB_SIDECAR_ENABLE on);
#      otherwise main's DONE would never show up there, and -sc refuses.
#
# Not done in this mode: main's own SPIFFS image (the Sidecar flasher
# writes bootloader, partition table, OTA data and app), the eFuse check,
# and the crash bundle (both need main's port).
#
# Expects from boot.sh: IDF_ARGS, QUICK, QUICK_RESET, RUN_SEC, CAT_TIMEOUT.

if $QUICK_RESET; then
	printf -- '-sc with -qq: nothing to flash; use the Sidecar console'"'"'s "reset" instead.\n' >&2
	exit 2
fi
if [[ -z "${SIDECAR_PORT:-}" ]]; then
	printf -- '-sc: SIDECAR_PORT is unset. Set it in tools/config.\n' >&2
	exit 2
fi
if [[ ! -c "${SIDECAR_PORT}" ]]; then
	printf -- '-sc: SIDECAR_PORT is not a character device: %s\n' "${SIDECAR_PORT}" >&2
	exit 2
fi

./tools/updateGitH.sh
./tools/power_off.sh

# 1. main
if ! $QUICK; then
	rm -rf ./build
fi
idf.py "${IDF_ARGS[@]}" --ccache build
if ! grep -q '^#define CONFIG_A8BB_LOG_VIA_SIDECAR 1' build/config/sdkconfig.h; then
	printf -- '-sc: this main build has no CONFIG_A8BB_LOG_VIA_SIDECAR (CONFIG_A8BB_SIDECAR_ENABLE off?): its output, DONE included, would never reach the Sidecar'"'"'s console.\n' >&2
	exit 2
fi

# 2. The Sidecar app (its own build dir and sdkconfig; main's -D args don't apply).
idf.py -C sidecar --ccache build
idf.py -C sidecar -b 1152000 flash -p "${SIDECAR_PORT}"

# 3. Its spiffs, with main's images staged in main/.
MAIN_BUILD=./build ./sidecar/flash_spiffs.sh -p "${SIDECAR_PORT}"

# 4-6. The Sidecar's console: send "flash" once it's up, power on after the
# flasher is done, stop at main's DONE (or a refusal/failure).
sc_log="$(mktemp)"
helper_pid=""
cleanup_sidecar_boot() {
	# "|| true": boot.sh runs with -e, and a failed kill (the helper long
	# gone) would make the exit trap replace the script's status with 1.
	if [[ -n "${helper_pid}" ]]; then kill "${helper_pid}" 2>/dev/null || true; fi
	rm -f -- "${sc_log}"
}
trap cleanup_sidecar_boot EXIT

(
	# The Sidecar logs "sidecar: 1s:" every second once its loop runs, and
	# reads console commands in that loop.
	for _ in $(seq 1 300); do
		grep -aq 'sidecar: 1s:' "${sc_log}" && break
		sleep 0.2
	done
	printf 'boot.sh -sc: sending "flash" to the Sidecar\n'
	printf 'flash\n' > "${SIDECAR_PORT}"
	for _ in $(seq 1 1200); do
		if grep -aq 'mainFlasher: done in' "${sc_log}"; then
			sleep 4
			./tools/power_on.sh
			exit 0
		fi
		grep -aqE 'mainFlasher: (refusing|no answer|nothing to flash|UART setup failed)|mainFlasher: [A-Z_]+ in [0-9]+ ms' "${sc_log}" && exit 0
		sleep 0.2
	done
) &
helper_pid=$!

# The Sidecar's console never goes idle (its per-second stats), so -t's
# run time bounds the wait as well as cat_until's idle timeout.
overall=()
if [[ -n "$RUN_SEC" ]]; then
	overall=(timeout $((RUN_SEC + 300)))
fi
cat_status=0
(while sleep .1; do if [[ -c "${SIDECAR_PORT}" ]]; then stty -F "${SIDECAR_PORT}" -echo raw || exit; cat "${SIDECAR_PORT}" || exit; fi; done) \
	| tee -a "${sc_log}" \
	| "${overall[@]}" ./tools/cat_until.sh -t "${CAT_TIMEOUT}" \
		"^(DONE)|(Rebooting)|mainFlasher: (refusing|no answer|nothing to flash|UART setup failed)|mainFlasher: [A-Z_]+ in [0-9]+ ms" \
		"${SIDECAR_PORT}" || cat_status=$?

if ! grep -aq 'mainFlasher: done in' "${sc_log}"; then
	printf -- '-sc: the Sidecar did not flash main (see its "mainFlasher:" lines above)\n' >&2
	exit 3
fi
if ! grep -aq '^DONE' "${sc_log}"; then
	printf -- '-sc: main flashed, but no DONE on the Sidecar'"'"'s console (cat_until status %d)\n' "${cat_status}" >&2
	exit "$(( cat_status ? cat_status : 4 ))"
fi
exit 0
