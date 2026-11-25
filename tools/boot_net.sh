# Sourced by tools/boot.sh for -net <host>: build main and the Sidecar app,
# and update and boot both through the Sidecar's web server, with no USB:
# for a test machine whose boards are only on the network.
#
#   1. Build main (the same -D args as boot.sh's) and the Sidecar app (its
#      SIDECAR_DEFINES, main's BOOT_CONFIG), as -sc does.
#   2. Upload both images, gzipped, to the Sidecar (curl POST /files/put
#      into NET_UPDATE_DIR, default /spiffs/update: main.bin.gz,
#      sidecar.bin.gz), each checked by the SHA-256 the Sidecar replies.
#   3. Its console's "update" (POST /console/cmd): main by OTA, then the
#      Sidecar itself, which restarts and resets main; or, both already
#      current, a plain "reset" of main.
#   4. Follow the Sidecar's console (GET /console/out), main's output
#      included, to main's DONE (as -sc reads it over USB), through the
#      Sidecar's restart; power the Atari on at main's BOOT COMPLETE or the
#      Sidecar's BOOT_EXPECTED_COMPLETE (never during an update).
#
# Exit status: 3 if the update failed (UPDATE_FAILED, MAIN_UPDATE_FAILED) or
# main never ran this build's app, 4 if no DONE (the Sidecar unreachable,
# or the run's time out), 5 if main runs another app, 6 after main crashed
# (its core is left in the Sidecar's spiffs, and fetched over the web into
# the capture as <prefix>.core.gz), 7 if main went silent on the link, 2 on
# bad use; else 0.
#
# Expects from boot.sh: NET_HOST, IDF_ARGS, RUN_SEC, CAT_TIMEOUT.
# Not done here: main's own SPIFFS, bootloader and partition table, the
# Sidecar's bootloader and partition table and spiffs (OTA installs apps
# only: those need one USB flash, boot.sh -sc), and the eFuse check.

NET_URL="http://${NET_HOST}"
NET_UPDATE_DIR="${NET_UPDATE_DIR:-/spiffs/update}"

net_up=false
for _ in 1 2 3 4 5 6; do   # a minute: the Sidecar may be (re)starting, the network slow
	curl -sf -m 10 -o /dev/null "${NET_URL}/files/status" && { net_up=true; break; }
	sleep 2
done
if ! ${net_up}; then
	printf -- '-net: the Sidecar at %s does not answer (GET /files/status)\n' "${NET_URL}" >&2
	exit 4
fi

./tools/power_off.sh
./tools/updateGitH.sh

# 1. Both apps, as boot_sidecar.sh builds them.
./tools/clear_build_defines.sh build "${IDF_ARGS[@]}"
idf.py "${IDF_ARGS[@]}" --ccache build
./tools/elf_archive.sh   # by SHA, for decoding a core of this build later
if grep -qE '^SIDECAR_LOOPBACK:[A-Z]+=(ON|1|TRUE|YES)$' build/CMakeCache.txt; then
	printf -- '-net: this main build has SIDECAR_LOOPBACK: its output would never reach the Sidecar.\n' >&2
	exit 2
fi
read -r -a SIDECAR_ARGS <<< "${SIDECAR_DEFINES:-}"
for arg in "${IDF_ARGS[@]}"; do
	[[ "${arg}" == -DI2SLINK_STATS_LOG=* ]] && SIDECAR_ARGS+=("${arg}")
done
if [[ -f sidecar/build/CMakeCache.txt ]]; then
	for knob in SLOT_TUNE SLOT_FAST_US SLOT_TRACE ERASE_CORE I2SLINK_STATS_LOG BOOT_TOKEN TEST_UNHEALTHY; do
		# BOOT_TOKEN among boot.sh's own -D args is main's: the Sidecar's comes from SIDECAR_DEFINES only.
		given=" ${SIDECAR_DEFINES:-} ${IDF_ARGS[*]} "
		[[ "${knob}" == BOOT_TOKEN ]] && given=" ${SIDECAR_DEFINES:-} "
		if [[ "${given}" != *" -D${knob}="* ]] && grep -q "^${knob}:" sidecar/build/CMakeCache.txt; then
			cmake -U "${knob}" sidecar/build > /dev/null 2>&1
		fi
	done
fi
boot_config="$(sed -n 's/^BOOT_CONFIG:[A-Z]*=//p' build/CMakeCache.txt)"
printf -- '-net: BOOT_CONFIG for both apps: %s\n' "${boot_config:-(default)}"
idf.py -C sidecar "${SIDECAR_ARGS[@]}" "-DBOOT_CONFIG=${boot_config}" --ccache build

# This build's app tokens (the first 4 bytes of the ELF SHA-256 in the app
# description, offset 0xb0 of the image; little endian, as printed).
want_app="$(od -An -tx4 -j 176 -N4 build/atari8EspBusBang.bin | tr -d ' ')"
want_sidecar="$(od -An -tx4 -j 176 -N4 sidecar/build/sidecar.bin | tr -d ' ')"

# 2. Upload, checked by the Sidecar's SHA-256 of what it received.
net_tmp="$(mktemp -d)"
net_log="${net_tmp}/console.log"
power_pid=""
cleanup_net_boot() {
	if [[ -n "${power_pid}" ]]; then kill "${power_pid}" 2>/dev/null || true; fi
	rm -rf -- "${net_tmp}"
}
trap cleanup_net_boot EXIT
gzip -n -c build/atari8EspBusBang.bin > "${net_tmp}/main.bin.gz"
gzip -n -c sidecar/build/sidecar.bin > "${net_tmp}/sidecar.bin.gz"
for f in main.bin.gz sidecar.bin.gz; do
	want_sha="$(sha256sum "${net_tmp}/${f}" | cut -c1-64)"
	# The old image first: the upload then needn't fit beside it (.part, then
	# renamed), and spiffs reclaims its pages sooner (a garbage collection in
	# the middle of an upload can stall it past the server's receive
	# timeouts). One retry: a 500 once in a 12-run soak (2026-11-25).
	uploaded=false
	for attempt in 1 2; do
		curl -sS -m 30 -o /dev/null -X POST "${NET_URL}/files/rm?p=${NET_UPDATE_DIR}/${f}" 2>/dev/null || true
		if reply="$(curl -sS --fail-with-body -m 300 --data-binary "@${net_tmp}/${f}" "${NET_URL}/files/put?p=${NET_UPDATE_DIR}/${f}" 2>&1)"; then
			uploaded=true
			break
		fi
		printf -- '-net: upload of %s failed (attempt %d): %s\n' "${f}" "${attempt}" "${reply}" >&2
	done
	${uploaded} || exit 4
	if [[ "${reply}" != *"\"sha256\":\"${want_sha}\""* ]]; then
		printf -- '-net: %s arrived damaged: sent sha256 %s, the Sidecar has %s\n' "${f}" "${want_sha}" "${reply}" >&2
		exit 4
	fi
	printf -- '-net: uploaded %s to %s (%s)\n' "${f}" "${NET_UPDATE_DIR}" "${reply}"
done

# The console from now on: everything after this offset.
since="$(curl -sS -f -m 10 -D - -o /dev/null "${NET_URL}/console/out" | tr -d '\r' | sed -n 's/^X-Console-Next: *//Ip')"
since="${since:-0}"
: > "${net_log}"

console_cmd() {
	curl -sS -f -m 10 --data-binary "$1" "${NET_URL}/console/cmd" > /dev/null
}

# 3. The update (main, then the Sidecar).
printf -- '-net: main %s, Sidecar %s: "update %s"\n' "${want_app}" "${want_sidecar}" "${NET_UPDATE_DIR}"
console_cmd "update ${NET_UPDATE_DIR}" || { printf -- '-net: the update command was refused\n' >&2; exit 4; }

# 4. The power-on waiter, on the console as captured (see boot_sidecar.sh).
./tools/power_on_when_up.sh "${net_log}" 600 'is the staged app: current' &
power_pid=$!

# Follow the console. A restarted Sidecar starts its offsets again from 0:
# a reply's next offset below ours means it did, so read it from its start.
deadline=$((SECONDS + ${RUN_SEC:-600} + 900))   # the updates, the run, a core fetch
reset_sent=false
done_at=0
status=4
headers="${net_tmp}/h"
while ((SECONDS < deadline)); do
	if ! curl -sS -m 10 -D "${headers}" -o "${net_tmp}/chunk" "${NET_URL}/console/out?since=${since}&wait=2000" 2>/dev/null; then
		sleep 1   # restarting, or the network: keep asking
		continue
	fi
	next="$(tr -d '\r' < "${headers}" | sed -n 's/^X-Console-Next: *//Ip')"
	[[ -n "${next}" ]] || { sleep 1; continue; }
	if ((next < since)); then
		printf -- '-net: the Sidecar restarted; its console from the start\n'
		since=0
		continue
	fi
	since="${next}"
	if [[ -s "${net_tmp}/chunk" ]]; then
		cat "${net_tmp}/chunk"
		cat "${net_tmp}/chunk" >> "${net_log}"
	fi
	if grep -aqE '^UPDATE_FAILED|MAIN_UPDATE_FAILED' "${net_log}"; then
		status=3
		break
	fi
	# Nothing to update: no restart, no reset by the Sidecar; reset main for the run.
	if ! ${reset_sent} && grep -aq '^UPDATE_DONE: nothing to do' "${net_log}"; then
		console_cmd reset && reset_sent=true
	fi
	# main's DONE (or the Sidecar's, for main), then up to 10 s more for the
	# sampler's last report (see boot_sidecar.sh); a failure exit reads on to
	# the Sidecar's fetch of the core.
	if ((done_at == 0)) && grep -aqE '^DONE ' "${net_log}"; then
		done_at=${SECONDS}
	fi
	if ((done_at)); then
		if grep -aE '^DONE [0-9.]+ +-[0-9]' "${net_log}" | grep -aqv ' Sidecar: '; then
			grep -aqE '^DONE [0-9.]+ +-?[0-9]+ Sidecar:|COREFILE_FETCH_FAILED|COREFILE_CORRUPT|ERASE_CORE: erased' "${net_log}" && break
		elif grep -aq '^SAMPLER_PERIOD_END .* stop' "${net_log}" || ((SECONDS - done_at >= 10)); then
			break
		fi
	fi
done
kill "${power_pid}" 2>/dev/null || true
power_pid=""

if ((status == 3)); then
	printf -- '-net: the update failed (above)\n' >&2
	exit 3
fi
if ! grep -aqE '^DONE ' "${net_log}"; then
	printf -- '-net: no DONE on the Sidecar'"'"'s console\n' >&2
	exit 4
fi
# A crash: the core stays in the Sidecar's spiffs; fetch it into the capture.
core_line="$(grep -a '^COREFILE_AVAILABLE main ' "${net_log}" | tail -1)"
if [[ -n "${core_line}" ]]; then
	read -r _ _ core_path _ _ <<< "${core_line}"
	if [[ -n "${RUN_CAPTURE_PREFIX:-}" ]] && curl -sS -f -m 600 -o "${RUN_CAPTURE_PREFIX}.core.gz" "${NET_URL}/files/get?p=${core_path}"; then
		printf -- '-net: main crashed; its core: %s.core.gz (gunzip it; decode against build/atari8EspBusBang.elf)\n' "${RUN_CAPTURE_PREFIX}"
	else
		printf -- '-net: main crashed; its core is %s on the Sidecar (GET /files/get?p=%s)\n' "${core_path}" "${core_path}"
	fi
	exit 6
fi
if grep -aqE '^(COREFILE_FETCH_FAILED|COREFILE_CORRUPT)' "${net_log}"; then
	printf -- '-net: main crashed; its core could not be fetched\n' >&2
	exit 6
fi
got_app="$(grep -ao 'main app [0-9a-f]* is the staged app: current' "${net_log}" | tail -1 | awk '{print $3}')"
if [[ -z "${got_app}" ]]; then
	printf -- '-net: main never ran the staged app\n' >&2
	exit 3
fi
if [[ "${got_app}" != "${want_app}" ]]; then
	printf -- '-net: main ran app %s, but this build is %s\n' "${got_app}" "${want_app}" >&2
	exit 5
fi
if grep -aqE '^DONE [0-9.]+ +-[0-9]+ Sidecar: main silent' "${net_log}"; then
	printf -- '-net: main went silent on the link (MAIN_LINK_SILENT above)\n' >&2
	exit 7
fi
printf 'boot.sh -net: main runs %s, the Sidecar %s\n' "${want_app}" "${want_sidecar}"
exit 0
