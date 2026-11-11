#!/bin/bash
# Build, flash and monitor the Sidecar profiler on the Sidecar board
# (SIDECAR_PORT, as for sidecar/run_sidecar.sh), recording the output under
# sidecar_profiler/run-logs. Extra arguments go to idf.py, e.g.
#   sidecar_profiler/run_sidecar_profiler.sh -DSIDECAR_PROFILER_SELFTEST=1
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
RUN_CAPTURE_TOOL="${RUN_CAPTURE_TOOL:-${PROJECT_ROOT}/tools/run_capture.sh}"

cd "$PROJECT_ROOT"
. ./tools/export_build_env.sh
. ./tools/export_boot_env.sh

cd ./sidecar_profiler
${RUN_CAPTURE_TOOL} --repo "$PROJECT_ROOT" --output-dir "./run-logs" -- idf.py -b 1152000 --ccache "$@" -p ${SIDECAR_PORT} app flash monitor
