#!/bin/bash

cd `dirname $0`
cd ..

while sleep .1; do ./tools/testpool_run.sh; done
