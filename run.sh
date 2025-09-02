#!/bin/bash 
make
if [ "$1" != "p" ]; then
	( sleep 3 && killall atari800 && killall hello_fuse ) &
fi
( sleep 1 && ./atari800.sh )&
./hello_fuse -f ./mnt
atr ./fuse_d1.atr get foo
atr ./fuse_d1.atr get bad
ls -l bad hostdir/BAD
diff bad hostdir/BAD
ls -l foo hostdir/FOO
diff foo hostdir/FOO

