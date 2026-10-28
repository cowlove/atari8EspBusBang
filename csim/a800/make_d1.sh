#!/bin/bash
#
set -e

DISK=./d1_test.atr
rm -f $DISK 
atr $DISK mkfs dos2.0d
atr $DISK put -l autoexec.bat 
cat /dev/random | od | head -256 > foo
cat /dev/random | od | head -2212 > big
echo -n 1 > onebyte
atr $DISK put -l foo 
atr $DISK put -l big  
atr $DISK put -l onebyte
atr $DISK put -l config.sys
atr $DISK put cmphalt.EXE

#atr $DISK rm autoexec.bat

