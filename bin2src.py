#!/usr/bin/python3

from sys import argv
import numpy as np

data = np.fromfile(argv[1], dtype=np.uint8)
#print("uint8_t rom[] = {")
bytes = 0;
for i in data:
    print("0x%02x, " % i,end='')
    bytes = bytes + 1
    if (bytes % 16 == 0): print("")
print("")
