# Sourced by tools/boot.sh for -sc: build main, and update and boot it
# through the Sidecar instead of over main's own USB port.
#
#   1. Build main (idf.py build, the same -D args; nothing flashed on PORT).
#   2. Build the Sidecar app and flash it on SIDECAR_PORT (tools/config).
#   3. Stage main's images in the Sidecar's spiffs (sidecar/flash_spiffs.sh,
#      MAIN_BUILD=./build) and flash that, last, so the Sidecar boots with
#      both. Staging a new app.bin.gz is the deliberate act that updates main.
#   4. Once the Sidecar's console is up, send it "reset": main reboots and
#      sends its boot report (BootStatus). The Sidecar compares main's app
#      (the ELF SHA-256 token, appVersion) with the staged app.bin.gz's; if they
#      differ it has main install the staged app by OTA over the link
#      (FetchFirmware), and main restarts into it and reports again. Never the
#      serial flasher: that's a manual step, for new boards, with the Atari
#      disconnected (the console's "flash"). Same source, same image (no
#      build time in the app description), so an unchanged main is left
#      alone; -DBOOT_TOKEN=<n> forces a new image.
#   5. With power control (MQTT_HOST/TAS), the Atari is off until the
#      Sidecar says main runs the staged app ("current"), then on, as boot.sh
#      does after a flash.
#   6. cat_until on the Sidecar's console: main's LOG() output reaches it
#      over the link (SerialOut), so main must be built with
#      CONFIG_A8BB_LOG_VIA_SIDECAR (CONFIG_A8BB_SIDECAR_ENABLE on);
#      otherwise main's DONE would never show up there, and -sc refuses.
#
# Exit status: 6 after a crash (main's, fetched by the Sidecar, or the
# Sidecar's own: tools/crash_bundle.sh --sidecar bundles it), 3 if main
# never ran the staged app ("current" never seen, or MAIN_UPDATE_FAILED), 4
# if no DONE, 5 if the app main ran isn't this build's (by token), else
# cat_until's.
#
# Not done in this mode: main's own SPIFFS image, its bootloader and
# partition table (OTA installs the app only), and the eFuse check.
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

# 2. The Sidecar app (its own build dir and sdkconfig; main's -D args don't
# apply), with main's boot profile: both apps build main/sysConfig.cpp and
# load BOOT_CONFIG's settings (the Sidecar takes its network settings from
# it). The value main's build has, cached or given now; none: the default.
boot_config="$(sed -n 's/^BOOT_CONFIG:[A-Z]*=//p' build/CMakeCache.txt)"
printf -- '-sc: BOOT_CONFIG for both apps: %s\n' "${boot_config:-(default)}"
idf.py -C sidecar --ccache "-DBOOT_CONFIG=${boot_config}" build
idf.py -C sidecar -b 1152000 flash -p "${SIDECAR_PORT}"

# 3. Its spiffs, with main's images staged in main/.
MAIN_BUILD=./build ./sidecar/flash_spiffs.sh -p "${SIDECAR_PORT}"

# This build's app token: what main reports as appVersion once it runs it
# (the first 4 bytes of the ELF SHA-256 in the app description, offset
# 0xb0 in the image; little endian, as the Sidecar prints it).
want_app="$(od -An -tx4 -j 176 -N4 build/atari8EspBusBang.bin | tr -d ' ')"

# 4-6. The Sidecar's console: reset main once it's up, power on once main
# runs the staged app, stop at main's DONE (or a failure).
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
	printf 'boot.sh -sc: resetting main through the Sidecar (app %s staged)\n' "${want_app}"
	printf 'reset\n' > "${SIDECAR_PORT}"
	for _ in $(seq 1 1500); do
		if grep -aq 'is the staged app: current' "${sc_log}"; then
			./tools/power_on.sh
			exit 0
		fi
		grep -aq 'MAIN_UPDATE_FAILED' "${sc_log}" && exit 0
		sleep 0.2
	done
) &
helper_pid=$!

# The Sidecar's console never goes idle (its per-second stats), so -t's
# run time bounds the wait as well as cat_until's idle timeout.
overall=()
if [[ -n "$RUN_SEC" ]]; then
	overall=(timeout $((RUN_SEC + 600)))   # a full core dump fetch can take minutes
fi
cat_status=0
(while sleep .1; do if [[ -c "${SIDECAR_PORT}" ]]; then stty -F "${SIDECAR_PORT}" -echo raw || exit; cat "${SIDECAR_PORT}" || exit; fi; done) \
	| tee -a "${sc_log}" \
	| "${overall[@]}" ./tools/cat_until.sh -t "${CAT_TIMEOUT}" \
		"^(DONE)|(Rebooting)|MAIN_UPDATE_FAILED" \
		"${SIDECAR_PORT}" || cat_status=$?

# A crash: main's (the Sidecar fetched its core into its spiffs:
# COREFILE_AVAILABLE, then its DONE) or the Sidecar's own (Backtrace:).
# Bundle it as boot.sh does for main on its own console, then fail.
if grep -aqE '^(COREFILE_AVAILABLE|COREFILE_FETCH_FAILED|COREFILE_CORRUPT|Backtrace:)' "${sc_log}"; then
	./tools/crash_bundle.sh --sidecar "${SIDECAR_PORT}" "${RUN_CAPTURE_PREFIX:-}" || true
	printf -- '-sc: a crash (see above)\n' >&2
	exit 6
fi
if ! grep -aq 'is the staged app: current' "${sc_log}"; then
	printf -- '-sc: main never ran the staged app (see the Sidecar'"'"'s "main app" lines above)\n' >&2
	exit 3
fi
got_app="$(grep -ao 'main app [0-9a-f]* is the staged app: current' "${sc_log}" | tail -1 | awk '{print $3}')"
if [[ "${got_app}" != "${want_app}" ]]; then
	printf -- '-sc: main ran app %s, but this build is %s\n' "${got_app}" "${want_app}" >&2
	exit 5
fi
updated="$(grep -ac 'updating main by OTA' "${sc_log}" || true)"
printf 'boot.sh -sc: main runs this build (app %s; %s)\n' "${want_app}" \
	"$( ((updated)) && echo "installed by OTA" || echo "already current, not reflashed")"
if ! grep -aq '^DONE' "${sc_log}"; then
	printf -- '-sc: no DONE on the Sidecar'"'"'s console (cat_until status %d)\n' "${cat_status}" >&2
	exit "$(( cat_status ? cat_status : 4 ))"
fi
exit 0
