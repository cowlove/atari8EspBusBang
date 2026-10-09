#!/bin/bash

cd `dirname $0`
cd ..

while sleep .1; do ./tools/run.sh; done 
