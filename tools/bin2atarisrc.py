#!/usr/bin/python3
import sys
from sys import argv
import numpy as np
atari = False;
atari = True;
data = np.fromfile(argv[1], dtype=np.uint8)
#print("uint8_t rom[] = {")
b = 0;
for i in data:
    if atari:
        if (b % 16 == 0):
            sys.stdout.flush();
            sys.stdout.buffer.write(b'\x9b');
            sys.stdout.flush();
            print("%d DATA %d" % (1000 + b / 16, i),end="");
        else:
            print(",%d" % i,end="")
    else:
        print("%3d" % i,end="")
        if (b % 16 != 15):
            print ("\n", end="");
    b = b + 1

sys.stdout.flush();
sys.stdout.buffer.write(b'\x9b');
sys.stdout.buffer.write(b'\x9b');
sys.stdout.buffer.write(b'\x9b');
