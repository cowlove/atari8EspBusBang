#!/bin/bash
cd `dirname $0`/..
. ./tools/export_boot_env.sh

echo "screen 2" > ${PORT}
