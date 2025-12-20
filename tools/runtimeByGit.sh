#!/bin/bash
cd `dirname $0`/..
HOURS=$1
LASTTAG=""
TAGCOUNT=0
TMP="/tmp/`basename $0`.$$.dat"
rm -f "$TMP"
[ "$HOURS" == "" ] && HOURS=1
for f in $(find ./stash/ -name '20*.*.*.*' -mmin -$(( $HOURS * 60 )) -print | sort -n); do 
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
if  [ "$LASTTAG" != "$TAG" ]; then 
       TAGCOUNT=$(( $TAGCOUNT + 1 ))
fi
LASTTAG=$TAG
echo -n  "$TAGCOUNT $TAG " | tee -a "$TMP"

( grep DONE $BASE.output || echo ) | cut '-d ' -f 2 | tee -a "$TMP"
done

gnuplot -e "
    set term dumb; 
    set logscale y;
    unset key;
    set offsets 1,1,0,0;
    plot '$TMP' u 1:3;
"

rm -f "$TMP"

