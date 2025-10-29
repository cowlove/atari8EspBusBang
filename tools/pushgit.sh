#!/bin/bash
cd $(dirname $0)/..
FILES=$({ git diff --name-only ; git diff --name-only --staged ; } | \
	sort | uniq | grep main/)
scp $FILES tun-miner6:src/atari8EspBusBang.$1/main
