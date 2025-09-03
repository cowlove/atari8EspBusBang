#!/bin/bash 
rm -f hostdir/FOO?
make || exit
atr ./fuse_d1.atr rm foo
atr ./fuse_d1.atr rm bad
atr ./fuse_d1.atr rm done
rm -rf ./foo ./bad
if [ "$1" != "p" ]; then
	( until atr ./fuse_d1.atr ls -al | grep ' done '; do echo waiting for DONE; sleep 1; done; killall atari800; killall hello_fuse ) &
fi
( sleep 1 && ./atari800.sh )&
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./libsmb2/build/lib ./hello_fuse -f ./mnt
atr ./fuse_d1.atr get foo
atr ./fuse_d1.atr get bad
ls -l bad hostdir/BAD
#diff bad hostdir/BAD
ls -l foo hostdir/FOO
#diff foo hostdir/FOA
killall run.sh

