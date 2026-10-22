#!/bin/bash
cd `dirname $0`/..

PTEST=$1


BRANCH="$(git branch --show-current)"
BRANCH_TAG="${BRANCH//\//_}"
BASE=./out/timing${PTEST}.${BRANCH_TAG}
BASELINE=${BASE}.baseline.txt

FILE_A=$( ls -1tr ${BASE}*.txt | grep -v baseline | tail -$(( $2+1 )) | head -1 )
if [ -z $3 ]; then 
	FILE_B=$BASELINE
else
	FILE_B=$( ls -1tr ${BASE}*.txt | grep -v baseline | tail -$(( $3+1 )) | head -1 )
fi 

echo $FILE_A $FILE_B
ls -ltr $FILE_A $FILE_B
echo cat $( echo $FILE_A | sed 's/txt$/diff/' )
echo cat $( echo $FILE_B | sed 's/txt$/diff/' )
echo Green is baseline or 2nd optional test argument:
./tools/compareTimings.sh $FILE_A $FILE_B


