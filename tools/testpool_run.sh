#!/bin/bash
cd `dirname $0`
cd ..

HW_TYPE=`pwd | sed -E 's/[^.]*//' | tr -d .`
# 2026-11-27: every machine runs the sled's unit test, with main restarting
# (a system reset) under the 6502 parked in native RAM between cycles (30 a
# run), to compare how the parked 6502 survives the restart: on the 800XLs
# (.1/.2) it leaves the park loop on every restart. SOAK is the safe-reboot
# soak .3-.6 ran before (hours of it since the sled went hot);
# -DSLED_LOOP=10 alone: no restarts.
# 2026-11-27, later: every machine runs SOAK, a safe reboot a minute, to
# test the D5-proof stack wait (BCS *) on the 800XLs (.2/.3).
SOAK="-DRESET_AFTER=60 -DRESET_REPEAT=1"
LOOP="-DSLED_LOOP=10 -DSLED_LOOP_RESTART=30 -DSLED_RESTART_SYS=1"   # a system reset, not IDF's CPU reset
case "${HW_TYPE}" in
# -DBOOT_CONFIG=SDX_AUTO on every machine again (2026-11-26): without it
# (the configuration chosen at boot, else SysConfig::kFallback) the soak's
# reboots crashed on most machines; back to a known configuration while
# RESET_AFTER is soaked.
"1") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
"2") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;; #800XL
"3") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" -DBOARD_VERSION=2 ${SOAK} ;;   # board v2; 800XL
"4") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" -DBOARD_VERSION=2 ${SOAK} ;;   # main board version 2 (R/W on GPIO15)
"5") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} -DSIO_TRACE=16 ;;   # test 2026-11-28: mount on use with main's SIO replies logged (first-use opens lose SDX here)
"6") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
esac
