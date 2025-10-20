#!/bin/bash
cd `dirname $0`/..

echo -e "\033[H\033[2J" 
while sleep .01; do 
	echo -e "\033[H" 
	find ./stash -name '*.output' -mtime -1 -print | xargs egrep -a '(DONE)' | sort -n | tail -`tput lines` | tail -20
	find ./stash -name '*.output' -mtime -1 -print | xargs ls -1tr | xargs grep SCREEN ${OUT} | sort -n | tail -26 | head -26
	sleep 1
done

