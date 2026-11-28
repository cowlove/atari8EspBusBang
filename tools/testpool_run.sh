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
# 2026-11-28: with HOT_CACHE_TRAP (main/hotCacheTrap.h) on every machine for
# a while: both caches suspended while core 0 is hot, so any flash or PSRAM
# access from hot core 0 code panics at once with its PC.
SOAK="-DRESET_AFTER=60 -DRESET_REPEAT=1 -DHOT_CACHE_TRAP=1"
LOOP="-DSLED_LOOP=10 -DSLED_LOOP_RESTART=30 -DSLED_RESTART_SYS=1"   # a system reset, not IDF's CPU reset
case "${HW_TYPE}" in
# -DBOOT_CONFIG=SDX_AUTO on every machine again (2026-11-26): without it
# (the configuration chosen at boot, else SysConfig::kFallback) the soak's
# reboots crashed on most machines; back to a known configuration while
# RESET_AFTER is soaked.
"1") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
"2") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;; #800XL
"3") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" -DBOARD_VERSION=2 ${SOAK} ;;   # board v2; 800XL
"4") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" -DBOARD_VERSION=2 ${SOAK} -DCOLD_CORE1_STALL=0 ;;   # main board version 2 (R/W on GPIO15); test 2026-11-28: core 1 left running through cold sections (is the 130XE .4 sensitive too?)
# .5, 2026-11-28: SIO_FIRST_TEST 7 then 8 (pbi.cpp), ~6 boots each: eager
# opens, core 1 left running, then on D1's first command a 184 KB
# esp_partition_read() burst with core 1 running (7) or stalled (8), core 1
# probed around it; BMON_DELAY_LOG for core 0's backlog after.
"5") K5="-DSIO_MOUNT_ON_USE=0 -DCOLD_CORE1_STALL=0 -DBMON_DELAY_LOG=1"
     tools/boot.sh     -p -t 400 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ${K5} -DSIO_FIRST_TEST=7
     tools/boot.sh     -p -t 400 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ${K5} -DSIO_FIRST_TEST=8 ;;
"6") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${SOAK} ;;
esac
