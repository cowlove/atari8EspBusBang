#!/bin/bash
cd `dirname $0`
cd ..

HW_TYPE=`pwd | sed -E 's/[^.]*//' | tr -d .`
GLOBAL="-DSLED_TEST=1 -DRESET_AFTER=300 -DRESET_REPEAT=1"
case "${HW_TYPE}" in
# No -DBOOT_CONFIG: each boot takes the configuration the Sidecar's (or, on
# the standalone ones, main's own) config/next or config/default names, else
# SysConfig::kFallback (SDX_AUTO), so the web page and the console can boot
# others. (.1 and .2 ran SDX_600XL before, forcing 64K base RAM on their
# 800XLs.)
"1") tools/boot.sh     -p -t 20000 ${GLOBAL} ;; #800XL
"2") tools/boot.sh     -p -t 20000 ${GLOBAL} ;; #800XL
"3") tools/boot.sh -sc -p -t 20000 ${GLOBAL} ;;
"4") tools/boot.sh -sc -p -t 20000 ${GLOBAL} ;;
"5") tools/boot.sh     -p -t 20000 ${GLOBAL} ;;
"6") tools/boot.sh -sc -p -t 20000 ${GLOBAL} ;;
esac
