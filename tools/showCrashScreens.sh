#!/bin/bash
cd `dirname $0`
cd ..

TAIL=$1
if [ "$TAIL" == "" ]; then TAIL=20; fi
CRASHES=`ls -1tr stash/*.output | tail -${TAIL} | xargs egrep -l "DONE(.*)-[0-9]"`
for f in $CRASHES; do
	echo
	echo
	echo less `pwd`/$f
	egrep "(SCREENB)|(DONE)" $f
	echo -n "00FF reads in bmon: "
	grep "R 00ff " $f | wc -l
	echo -n "smb2 errors: "
	grep "smb2" $f | wc -l
done


