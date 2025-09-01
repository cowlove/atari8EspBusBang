#!/bin/bash
atari800  -xl -stretch 3 \
	-turbo \
	-xlxe_rom ~/atari800/REV03.ROM -basic_rom ~/atari800/REVC.ROM  \
	-cart ~/atari800/SDX450_maxflash1.car  \
	~/src/fusepy/fuse_d1.atr ~/src/fusepy/mnt/fuse_d2.img

