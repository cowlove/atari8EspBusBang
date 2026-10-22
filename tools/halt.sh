#!/bin/bash
cd `dirname $0`/..
. ./tools/export_boot_env.sh

echo "exit 2" > ${PORT}
