#!/bin/bash
cd `dirname $0`
cd ..
LAST=`ls -1tr ./stash/*.output | tail -1`
if [ "$1" == "f" ]; then 
	ls -1tr ./stash/*.output | xargs grep -a DONE  | tail -`tput lines`
else
        ls -1tr ./stash/*.output | xargs grep -ah DONE  | tail -`tput lines`
fi
echo File age: $(( $(date '+%s') - $(date -r "${LAST}" '+%s') ))

tail -1 ${LAST}
sleep 3
tail -1 ${LAST}



