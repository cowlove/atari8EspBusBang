#!/bin/bash
#
set -e
atr d1.atr put -l autoexec.bat
time build-atari/atari800-csim  -nopatchall -xl -cart ~/atari800/SDX450_maxflash1.car -stretch 3

