#!/bin/bash
cd `dirname $0`/..
. ./tools/config

echo "exit 2" > ${PORT}
