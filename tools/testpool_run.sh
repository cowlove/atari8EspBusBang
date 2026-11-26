#!/bin/bash
cd `dirname $0`
cd ..

HW_TYPE=`pwd | sed -E 's/[^.]*//' | tr -d .`
GLOBAL="-DSLED_TEST=1"
case "${HW_TYPE}" in
"1") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_600XL\"  ${GLOBAL} ;; #800XL, using 600XL to force baseMem=64K  
"2") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_600XL\"  ${GLOBAL} ;; #same 
"3") tools/boot.sh -sc -p -t 20000  '-DBOOT_CONFIG="SDX_AUTO"'  ${GLOBAL} ;;
"4") tools/boot.sh -sc -p -t 20000 '-DBOOT_CONFIG="SDX_AUTO"'   ${GLOBAL} ;;
"5") tools/boot.sh     -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\"   ${GLOBAL} ;;
"6") tools/boot.sh -sc -p -t 20000 -DBOOT_CONFIG=\"SDX_AUTO\"   ${GLOBAL} ;;
esac
