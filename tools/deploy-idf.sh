#!/bin/bash -e
#
idf.py build
echo "$(git describe --abbrev=6 --dirty --always)" > build/git.version.txt
git diff > build/git.diff
find build -name *.bin | xargs tar czvf build/build.tgz build/git.*  
scp build/build.tgz tun-miner6:/tmp/build.tgz
ssh tun-miner6 "cd /home/jim/tmp/ && tar xzvf /tmp/build.tgz"

