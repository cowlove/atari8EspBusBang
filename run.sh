#!/bin/bash 
make
( sleep 3 && killall atari800 && killall hello_fuse ) &
( sleep 1 && ./atari800.sh )&
./hello_fuse -f ./mnt
atr ./fuse_d1.atr get foo
atr ./fuse_d1.atr get bad
ls -ltr bad BAD foo FOO
