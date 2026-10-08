#!/bin/bash
HW_ID=`pwd | sed -E 's/[^.]*//' | tr -d .`

case "${HW_ID}" in 
"1") while sleep .1; do tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XL\" ; done ;;
"2") while sleep .1; do tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XL\" ; done ;;
"3") while sleep .1; do tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_600XL\" ; done ;;
"4") while sleep .1; do tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_600XL\" ; done ;;
"5") while sleep .1; do tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XE192\" ; done ;;
"6") while sleep .1; do tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XE192\" ; done ;;
esac

