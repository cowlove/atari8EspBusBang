#!/bin/bash
cd `dirname $0`
cd ..
git diff > ./stash/sweepstart.git 

for sweep in $(seq 105 -1 80); do
   sed -i -E 's/SWEEP=[-]?[0-9]+/SWEEP='${sweep}/ main/hwType.h
   for rep in {1..10}; do sleep .1; ./tools/boot.sh -t -DRUNTIME_SEC=660 -DBOOT_CONFIG=\"STRESS\" ; done
done
