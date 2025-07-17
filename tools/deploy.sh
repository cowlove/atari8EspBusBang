#!/bin/bash -e 

make -C esp32
make -C esp32 pbirom.h 
make -C esp32 page6.h
cd ./esp32/ 
scp Makefile page6.h page6.asm ascii2keypress.h atari8EspBusBang.ino pbirom.asm pbirom.h core1.h core1.cpp tun-miner6:src/atari8EspBusBang/esp32/


