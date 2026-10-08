#!/bin/bash
for sweep in $(seq 68 1 105); do
   sed -i -E 's/SWEEP=[-]?[0-9]+/SWEEP='${sweep}/ main/core1defs.cpp
   for rep in {1..10}; do sleep .1; ./tools/boot.sh -DBOOT_CONFIG=\"HELLO_CARTINT\" ; done
done


