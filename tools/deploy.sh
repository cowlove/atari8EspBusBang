#!/bin/bash -e 

make -C main 
make -C main pbirom.h 
make -C main page6.h
cd ./main/ 
scp Makefile pbirom.* page6.* core1.* *.ino tun-miner6:src/atari8EspBusBang/esp32/


