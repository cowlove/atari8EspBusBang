#!/bin/bash
cd `dirname $0`
cd ..

HW_TYPE=`pwd | sed -E 's/[^.]*//' | tr -d .`
# 2026-11-27: .3-.6 soak safe reboots of main (they have run hours of them
# since the sled went hot); .1/.2 (800XLs, whose reboots die inside the
# ESP32's restart) run the sled's unit test, with main restarting under
# the parked 6502 between cycles (30 a run); -DSLED_LOOP=10 alone: no restarts.
SOAK="-DSLED_TEST=1 -DRESET_AFTER=300 -DRESET_REPEAT=1"
LOOP="-DSLED_TEST=1 -DSLED_LOOP=10 -DSLED_LOOP_RESTART=30"
case "${HW_TYPE}" in
# -DBOOT_CONFIG=SDX_AUTO on every machine again (2026-11-26): without it
# (the configuration chosen at boot, else SysConfig::kFallback) the soak's
# reboots crashed on most machines; back to a known configuration while
# RESET_AFTER is soaked.
"1") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${LOOP} ;; #800XL
"2") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${LOOP} ;; #800XL
"3") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
"4") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
"5") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
"6") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
esac
