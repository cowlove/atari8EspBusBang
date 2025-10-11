#!/bin/bash
cd "$(dirname $0)/.."
. ./tools/config
LA_DIR=/home/jim/opt/Dall-in-one_6.0.0.1-linux-x64.zip/
TAG=`date +%Y%m%d.%H%M%S`
OUT=./traces/${TAG}
#mkdir -f ./traces
ln -s ../stash/`ls -1tr stash | grep .output | tail -1` ${OUT}.output
git diff > ${OUT}.git_diff
echo "$(git describe --abbrev=6 --dirty --always)" >> ${OUT}.git_version
GIT="$(git describe --abbrev=6 --dirty --always)"
BRANCH="$(git branch --show-current)"

${LA_DIR}/TerminalCapture capture ${BUSA} tools/cap24.tcs /tmp/tc.csv \
	&& cat /tmp/tc.csv \
	| tr ',' ' ' \
	| tail -n +2  > ${OUT}.cap
       
tools/analyze.py ${OUT}.cap | tee ${OUT}.ana

cp ${OUT}.ana ./traces/ana.${BRANCH}.${GIT}.dat
cp ${OUT}.ana ./traces/ana.${BRANCH}.dat


