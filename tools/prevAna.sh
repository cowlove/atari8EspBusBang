#!/bin/bash
#!/bin/bash
cd `dirname $0`/..

F=./traces/$(ls -1tr ./traces/ | grep .ana| tail -$(( 1 + $1 ))  | head -1)
cat $F
echo $F

