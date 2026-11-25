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
# -qq: none of 1-3 (nothing built or flashed): the Sidecar is reset over its
# USB, then 4-6 as usual (main reset through it, its output captured).
#   6. cat_until on the Sidecar's console: main's LOG() output reaches it
#      over the link (SerialOut), so main is built with -DSIDECAR=ON
#      (boot.sh adds it) and without SIDECAR_LOOPBACK (refused): otherwise
#      main's DONE would never show up there.
#
# SIDECAR_DEFINES (env): -D args for the Sidecar app's build, from its
# CMake list (sidecar/main/CMakeLists.txt: SLOT_TUNE, SLOT_FAST_US,
# SLOT_TRACE, I2SLINK_STATS_LOG: boot.sh's own -DI2SLINK_STATS_LOG=1 is
# passed on too); one not given is
# cleared from its cache, so none carries over to the next run.
#
# Exit status: 6 after a crash (main's, fetched by the Sidecar, or the
# Sidecar's own: tools/crash_bundle.sh --sidecar bundles it), 3 if main
# never ran the staged app ("current" never seen, or MAIN_UPDATE_FAILED), 4
# if no DONE, 5 if the app main ran isn't this build's (by token), 7 if
# main went silent on the link (the Sidecar's MAIN_LINK_SILENT: no command
# for the boot profile's ioTimeoutSec, and no core dump after a reset),
# else cat_until's.
#
# Not done in this mode: main's own SPIFFS image, its bootloader and
# partition table (OTA installs the app only), and the eFuse check.
#
# Expects from boot.sh: IDF_ARGS, QUICK, QUICK_RESET, RUN_SEC, CAT_TIMEOUT.

if [[ -z "${SIDECAR_PORT:-}" ]]; then
	printf -- '-sc: SIDECAR_PORT is unset. Set it in tools/config.\n' >&2
	exit 2
fi
if [[ ! -c "${SIDECAR_PORT}" ]]; then
	printf -- '-sc: SIDECAR_PORT is not a character device: %s\n' "${SIDECAR_PORT}" >&2
	exit 2
fi

./tools/power_off.sh

# Reset the Sidecar into its app: esptool's watchdog reset, not its hard
# reset. On the DevKitC carrier's N16R8 (2026-11-21; the jumper-wired pair
# too, 2026-11-20) a USB-JTAG hard reset after esptool has been talking to the
# chip comes back up in the ROM loader every time, and the run times out with
# a silent console; the watchdog reset boots the app.
sidecar_reset() {
	python -m esptool --chip esp32s3 -p "${SIDECAR_PORT}" --before default-reset --after watchdog-reset read-mac > /dev/null
	# The reset re-enumerates its USB device: wait for the port to come back.
	sleep 0.5
	for _ in $(seq 1 100); do [[ -c "${SIDECAR_PORT}" ]] && break; sleep 0.1; done
	# Raw, no echo, at once: with the tty's default echo on, the Sidecar's
	# boot log comes back into its console input until the capture loop
	# below sets this, and the "reset" command then arrives behind it.
	stty -F "${SIDECAR_PORT}" -echo raw
}

if $QUICK_RESET; then
	# -qq: nothing built or flashed. Reset the Sidecar (its USB-JTAG reset:
	# esptool's run), which then resets main as below: both boards boot
	# afresh, so the link starts from new initial conditions (soaks).
	if [[ ! -f build/atari8EspBusBang.bin ]]; then
		printf -- '-sc -qq: no main build to compare against (build/atari8EspBusBang.bin); run -sc first.\n' >&2
		exit 2
	fi
	sidecar_reset
else
./tools/updateGitH.sh

# 1. main. Not rm -rf build: an earlier run's -D settings are cleared
# instead (cache variables persist), and the build stays incremental.
./tools/clear_build_defines.sh build "${IDF_ARGS[@]}"
idf.py "${IDF_ARGS[@]}" --ccache build
./tools/elf_archive.sh   # by SHA, for decoding a core of this build later (crash_bundle.sh)
# boot.sh passed -DSIDECAR=ON; SIDECAR_LOOPBACK on top would keep LOG()
# off the link.
if grep -qE '^SIDECAR_LOOPBACK:[A-Z]+=(ON|1|TRUE|YES)$' build/CMakeCache.txt; then
	printf -- '-sc: this main build has SIDECAR_LOOPBACK: its output, DONE included, would never reach the Sidecar'"'"'s console.\n' >&2
	exit 2
fi

# 2. The Sidecar app (its own build dir and sdkconfig; main's -D args don't
# apply; SIDECAR_DEFINES does), with main's boot profile: both apps build
# main/sysConfig.cpp and load BOOT_CONFIG's settings (the Sidecar takes its
# network settings from it). The value main's build has, cached or given
# now; none: the default.
read -r -a SIDECAR_ARGS <<< "${SIDECAR_DEFINES:-}"
# -DI2SLINK_STATS_LOG=<n> among boot.sh's own -D args is the Sidecar's (its
# 20 s link stats): passed on to its build as well.
for arg in "${IDF_ARGS[@]}"; do
	[[ "${arg}" == -DI2SLINK_STATS_LOG=* ]] && SIDECAR_ARGS+=("${arg}")
done
if [[ -f sidecar/build/CMakeCache.txt ]]; then
	for knob in SLOT_TUNE SLOT_FAST_US SLOT_TRACE ERASE_CORE I2SLINK_STATS_LOG BOOT_TOKEN TEST_UNHEALTHY; do
		# Only a knob in the cache: an unconditional -U reconfigured every run.
		# BOOT_TOKEN among boot.sh's own -D args is main's: the Sidecar's comes from SIDECAR_DEFINES only.
		given=" ${SIDECAR_DEFINES:-} ${IDF_ARGS[*]} "
		[[ "${knob}" == BOOT_TOKEN ]] && given=" ${SIDECAR_DEFINES:-} "
		if [[ "${given}" != *" -D${knob}="* ]] && grep -q "^${knob}:" sidecar/build/CMakeCache.txt; then
			cmake -U "${knob}" sidecar/build > /dev/null 2>&1
		fi
	done
fi
boot_config="$(sed -n 's/^BOOT_CONFIG:[A-Z]*=//p' build/CMakeCache.txt)"
printf -- '-sc: BOOT_CONFIG for both apps: %s\n' "${boot_config:-(default)}"
idf.py -C sidecar "${SIDECAR_ARGS[@]}" "-DBOOT_CONFIG=${boot_config}" --ccache build
idf.py -C sidecar -b 1152000 flash -p "${SIDECAR_PORT}"

# 3. Its spiffs, with main's images staged in main/.
MAIN_BUILD=./build ./sidecar/flash_spiffs.sh -p "${SIDECAR_PORT}"
sidecar_reset   # flash_spiffs's hard reset can leave it in the ROM loader
fi

# This build's app token: what main reports as appVersion once it runs it
# (the first 4 bytes of the ELF SHA-256 in the app description, offset
# 0xb0 in the image; little endian, as the Sidecar prints it).
want_app="$(od -An -tx4 -j 176 -N4 build/atari8EspBusBang.bin | tr -d ' ')"

# 4-6. The Sidecar's console: reset main once it's up, power on once main
# runs the staged app and is up (its "BOOT COMPLETE" after "is the staged app:
# current": a boot that ends in an OTA doesn't count), stop at main's DONE
# (or a failure).
sc_log="$(mktemp)"
helper_pid=""
power_pid=""
cleanup_sidecar_boot() {
	# "|| true": boot.sh runs with -e, and a failed kill (the helper long
	# gone) would make the exit trap replace the script's status with 1.
	if [[ -n "${helper_pid}" ]]; then kill "${helper_pid}" 2>/dev/null || true; fi
	if [[ -n "${power_pid}" ]]; then kill "${power_pid}" 2>/dev/null || true; fi
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
	# The newline first: whatever already sits in the Sidecar's console line
	# (echoed log text, before the port went raw) becomes its own command.
	printf '\nreset\n' > "${SIDECAR_PORT}"
	for _ in $(seq 1 1500); do
		grep -aq 'is the staged app: current' "${sc_log}" && exit 0
		grep -aq 'MAIN_UPDATE_FAILED' "${sc_log}" && exit 0
		sleep 0.2
	done
) &
helper_pid=$!
# The power-on waiter: main's BOOT COMPLETE, or the Sidecar's
# BOOT_EXPECTED_COMPLETE (5 s after a normal boot report), after the boot
# report that finds the staged app current: never during an OTA or a core
# fetch, never blind; 2 minutes at most from now; killed on any exit.
./tools/power_on_when_up.sh "${sc_log}" 120 'is the staged app: current' &
power_pid=$!

# The Sidecar's console never goes idle (its per-second stats), so -t's
# run time bounds the wait as well as cat_until's idle timeout.
# After main's DONE, read on (-a/-w: the same reader, so nothing printed
# meanwhile is lost) to the Sidecar sampler's last report: main stops the
# sampler just before its exit report (SampleControl), and the Sidecar
# prints that period's histograms, ending "SAMPLER_PERIOD_END ... stop". At
# most 10 s (a paused or quiet sampler prints none); not at all if it came
# before DONE.
overall=()
if [[ -n "$RUN_SEC" ]]; then
	overall=(timeout $((RUN_SEC + 600)))   # a full core dump fetch can take minutes
fi
cat_status=0
(while sleep .1; do if [[ -c "${SIDECAR_PORT}" ]]; then stty -F "${SIDECAR_PORT}" -echo raw || exit; cat "${SIDECAR_PORT}" || exit; fi; done) \
	| tee -a "${sc_log}" \
	| "${overall[@]}" ./tools/cat_until.sh -t "${CAT_TIMEOUT}" \
		-a '^SAMPLER_PERIOD_END .* stop' -w 10 \
		"^(DONE)|(Rebooting)|MAIN_UPDATE_FAILED" \
		"${SIDECAR_PORT}" || cat_status=$?

# A failure exit (main's DONE <sec> -<n> ...) panics main after its report,
# for a core dump (main.cpp, EXIT_COREDUMP); main reboots, and the Sidecar
# fetches the core at its boot report: read on to its "DONE <sec> -11
# Sidecar: main crashed", so the crash check below bundles it. (The
# Sidecar's own DONE lines say "Sidecar:"; main's never do.)
if grep -aE '^DONE [0-9.]+ +-[0-9]' "${sc_log}" | grep -aqv ' Sidecar: '; then
	(while sleep .1; do if [[ -c "${SIDECAR_PORT}" ]]; then stty -F "${SIDECAR_PORT}" -echo raw || exit; cat "${SIDECAR_PORT}" || exit; fi; done) \
		| tee -a "${sc_log}" \
		| timeout 900 ./tools/cat_until.sh -t 600 "^DONE [0-9.]+ +-?[0-9]+ Sidecar:|COREFILE_FETCH_FAILED|COREFILE_CORRUPT|Backtrace:|ERASE_CORE: erased" \
			"${SIDECAR_PORT}" || true
fi

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
if grep -aqE '^DONE [0-9.]+ +-[0-9]+ Sidecar: main silent' "${sc_log}"; then
	printf -- '-sc: main went silent on the link (MAIN_LINK_SILENT above): hung, or lost the link\n' >&2
	exit 7
fi
exit 0
