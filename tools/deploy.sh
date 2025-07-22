#!/bin/bash -e 

make -C esp32
make -C esp32 pbirom.h 
make -C esp32 page6.h
cd ./esp32/ 
scp Makefile pbirom.* page6.* core1.* *.ino tun-miner6:src/atari8EspBusBang/esp32/


