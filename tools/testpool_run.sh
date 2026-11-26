#!/bin/bash
cd `dirname $0`
cd ..

HW_TYPE=`pwd | sed -E 's/[^.]*//' | tr -d .`
GLOBAL="-DSLED_TEST=1 -DRESET_AFTER=300 -DRESET_REPEAT=1"
case "${HW_TYPE}" in
# -DBOOT_CONFIG=SDX_AUTO on every machine again (2026-11-26): without it
# (the configuration chosen at boot, else SysConfig::kFallback) the soak's
# reboots crashed on most machines; back to a known configuration while
# RESET_AFTER is soaked.
"1") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${GLOBAL} ;; #800XL
"2") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${GLOBAL} ;; #800XL
"3") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${GLOBAL} ;;
"4") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${GLOBAL} ;;
"5") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${GLOBAL} ;;
"6") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\" ${GLOBAL} ;;
esac
