#!/bin/bash -ex
make
( sleep 1 && ./atari800.sh )&
./hello_fuse -f ./mnt
