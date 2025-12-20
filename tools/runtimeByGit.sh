#!/bin/bash

for f in $(find ./stash/ -name '20*.*.*.*' -mmin -$(( $1 * 120 )) -print | sort -n); do 
    BASE=$(echo $f | sed -E 's/[.][^.]+[.][^.]+$//')
    if [ ! -f $BASE.output ]; then 
        continue;
    fi
    TAG=$(echo $f | awk ' \
{
    if (match($0, /[^.]+[.][a-z0-9]+$/)) {
        print substr($0, RSTART, RLENGTH)
    }
}')
echo -n  "$TAG "
( grep DONE $BASE.output || echo ) | cut '-d ' -f 2
done


