#!/bin/bash
cd `dirname $0`
cd ..

HW_TYPE=`pwd | sed -E 's/[^.]*//' | tr -d .`

case "${HW_TYPE}" in
"1") tools/boot.sh -p -t 20000 -DBOOT_CONFIG=\"SDX_XL\" -DPROFILE_MODE=GPIO ;;
"2") tools/boot.sh -p -t 20000 -DBOOT_CONFIG=\"SDX_XL\" -DPROFILE_MODE=GPIO ;;
"3") tools/boot.sh -p -t 20000 -DBOOT_CONFIG=\"STRESS_MC\" -DPROFILE_MODE=GPIO  ;;
"4") tools/boot.sh -p -t 20000 '-DBOOT_CONFIG="SDX_600XL"' -DPROFILE_MODE=GPIO ;;
"5") tools/boot.sh -p -t 20000 -DBOOT_CONFIG=\"SDX_XE192\" -DPROFILE_MODE=GPIO ;;
"6") tools/boot.sh -p -t 20000 -DBOOT_CONFIG=\"SDX_XE192\" -DPROFILE_MODE=GPIO ;;
esac
