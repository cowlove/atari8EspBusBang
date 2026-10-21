#!/bin/bash
PTEST=$1


BRANCH="$(git branch --show-current)"
BRANCH_TAG="${BRANCH//\//_}"
BASE=./out/timing${PTEST}.${BRANCH_TAG}
BASELINE=${BASE}.baseline.txt

FILE=$( ls -1tr ${BASE}*.txt | grep -v baseline | tail -$(( $2+1 )) | head -1 )

echo $FILE $BASELINE
ls -ltr $FILE $BASELINE
echo cat $( echo $FILE | sed 's/txt$/diff/' )
echo cat $( echo $BASELINE | sed 's/txt$/diff/' )

./tools/compareTimings.sh $BASELINE $FILE


