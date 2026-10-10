#!/bin/bash
cd `dirname $0`
cd ..

HW_ID=`pwd | sed -E 's/[^.]*//' | tr -d .`

case "${HW_ID}" in 
"1") tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XL\" ;;
"2") tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XL\" ;;
"3") tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SIMPLEMEM_CART\" ;;
"4") tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SIMPLEMEM_CART\" ;;
"5") tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XE192\" ;;
"6") tools/boot.sh -DRUNTIME_SEC=20000 -DBOOT_CONFIG=\"SDX_XE192\" ;;
esac

