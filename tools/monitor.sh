#!/bin/bash
cd `dirname $0`
LAST=`ls -1tr ../stash/*.output | tail -1`
#while sleep 1; do 
	ls -1tr ../stash/*.output | xargs grep -h DONE  | tail -`tput lines`
	echo File age: $(( $(date '+%s') - $(date -r "${LAST}" '+%s') ))
#done
tail -1 ${LAST}
sleep 3
tail -1 ${LAST}



