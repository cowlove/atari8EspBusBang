#!/bin/bash
cd `dirname $0`/..

F=$(ls -1tr ./stash/*.output | tail -$(( 1 + $1 ))  | head -1)
cat $F
echo $F

