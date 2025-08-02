#!/bin/bash -e
#
idf.py build
find build -name *.bin | xargs tar czvf build/build.tgz 
scp build/build.tgz tun-miner6:/tmp/build.tgz
ssh tun-miner6 "cd /home/jim/tmp/ && tar xzvf /tmp/build.tgz"

