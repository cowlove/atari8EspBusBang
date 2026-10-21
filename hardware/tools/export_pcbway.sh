#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTPUT_DIR="${ROOT_DIR}/hardware/gerbers/pcbway"

if command -v kicad-cli >/dev/null 2>&1; then
	KICAD_CLI="$(command -v kicad-cli)"
elif command -v kicad.kicad-cli >/dev/null 2>&1; then
	KICAD_CLI="$(command -v kicad.kicad-cli)"
else
	echo "error: kicad-cli or kicad.kicad-cli is required (use KiCad 9 or newer)" >&2
	exit 1
fi

if ! command -v zip >/dev/null 2>&1; then
	echo "error: zip is required to package the fabrication files" >&2
	exit 1
fi

mkdir -p "$(dirname "${OUTPUT_DIR}")"
WORK_DIR="$(mktemp -d "${ROOT_DIR}/hardware/gerbers/.pcbway-export.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

boards=(
	"espBusBang/atariEspBusBang3.kicad_pcb"
	"eci2Pbi/atari8EciToPbi.kicad_pcb"
)

layers="F.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,B.Silkscreen,Edge.Cuts"

for board in "${boards[@]}"; do
	board_path="${ROOT_DIR}/hardware/${board}"
	board_name="$(basename "${board_path}" .kicad_pcb)"
	board_output="${WORK_DIR}/${board_name}"
	mkdir -p "${board_output}"

	echo "Exporting ${board_name}"
	"${KICAD_CLI}" pcb export gerbers \
		--layers "${layers}" \
		--no-x2 \
		--no-netlist \
		--output "${board_output}" \
		"${board_path}"

	"${KICAD_CLI}" pcb export drill \
		--format excellon \
		--drill-origin absolute \
		--excellon-units mm \
		--excellon-zeros-format decimal \
		--excellon-separate-th \
		--output "${board_output}" \
		"${board_path}"

	if ! compgen -G "${board_output}/*" >/dev/null; then
		echo "error: KiCad produced no fabrication files for ${board_name}" >&2
		exit 1
	fi
	(
		cd "${board_output}"
		zip -q "${WORK_DIR}/${board_name}.zip" ./*
	)
done

# Publish only after both boards export successfully, keeping previous results
# intact if KiCad fails partway through.
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"
for board in "${boards[@]}"; do
	board_name="$(basename "${board}" .kicad_pcb)"
	mv "${WORK_DIR}/${board_name}" "${OUTPUT_DIR}/"
	mv "${WORK_DIR}/${board_name}.zip" "${OUTPUT_DIR}/"
done

echo "PCBWay fabrication files are in ${OUTPUT_DIR}"
