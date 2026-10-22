#!/bin/bash
xterm -e atari800  -xl -stretch 3 \
	-cart SDX450_maxflash1.car  \
	-H1 ./h1 \
	-hreadwrite \
	./fuse_d1.atr ./mnt/fuse_d2.img \
	-turbo \

#	-xlxe_rom REV03.ROM -basic_rom REVC.ROM  \

