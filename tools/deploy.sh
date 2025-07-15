#!/bin/bash

make && make pbirom.h && make page6.h && scp page6.h page6.asm ascii2keypress.h atari8EspBusBang.ino pbirom.asm pbirom.h core1.h core1.cpp tun-miner6:src/espEmptyTemplate/

