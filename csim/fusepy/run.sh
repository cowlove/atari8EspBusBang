#!/bin/bash -x 
: "${CDEFS:=-DLOG=printf -DSECTOR_SIZE=256}"
export CDEFS
SMB_URL=${2:-smb://localhost/foo}
rm -f hostdir/FOO?
fusermount -u ./mnt
make clean
make || exit
rm -f ./fuse_d1.atr 
atr ./fuse_d1.atr mkfs dos2.0s
atr ./fuse_d1.atr put -l autoexec.bat 
atr ./fuse_d1.atr put -l FOO 
if [ "$1" = "-e" ]; then
	( until atr ./fuse_d1.atr ls -al | grep ' done ' > /dev/null; do sleep .1; done; sleep 1; killall atari800; killall hello_fuse ) &
fi
( sleep 1 && ./atari800.sh )&
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:./libsmb2/build/lib ./hello_fuse "$SMB_URL" -f ./mnt
atr ./fuse_d1.atr get foo
atr ./fuse_d1.atr get bad
ls -l bad hostdir/BAD
#diff bad hostdir/BAD
ls -l foo hostdir/FOO
#diff foo hostdir/FOA
ls -l hostdir
killall run.sh
