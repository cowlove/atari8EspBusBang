#!/bin/bash
cd $(dirname $0)/..
cd main
~/opt/llvm-mos/bin/mos-atari8-dos-clang ./lfs/hello.c -Oz -o ./lfs/hello.exe
atr lfs/llvm_d1.atr put ./lfs/hello.exe autorun.sys
atr lfs/d2.atr put ./lfs/hello.exe hello.exe

#~/src/atari800/src/
atari800  -rambo -stretch 3 -turbo  -xlxe_rom ~/atari800/REV03.ROM -basic_rom ~/atari800/REVC.ROM -basic ./lfs/llvm_d1.atr ./lfs/d2.atr 


