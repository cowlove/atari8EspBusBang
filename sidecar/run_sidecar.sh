#!/bin/bash
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
RUN_CAPTURE_TOOL="${RUN_CAPTURE_TOOL:-${PROJECT_ROOT}/tools/run_capture.sh}"

cd "$PROJECT_ROOT"
pwd
. ./tools/export_build_env.sh
. ./tools/export_boot_env.sh

cd ./sidecar
${RUN_CAPTURE_TOOL} --repo "$PROJECT_ROOT" --output-dir "./run-logs" -- idf.py -b 1152000 --ccache -p ${SIDECAR_PORT} app flash monitor

