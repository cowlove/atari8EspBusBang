#!/usr/bin/python

import sys
import re
a2k = {}
for i in range(0, 256):
    a2k[i] = 255

for l in sys.stdin:
    l = re.sub(r"[.:cdea]", "", l)
    l = re.sub(r"[.:cdea]", "", l)
    w = l.split()
    if (len(w) > 5):
        a2k[int(w[4])] = int(w[1]) 

print("static const uint8_t ascii2keypress[] = {")
for i in range(0, 256):
    print("%3d, " % (a2k[i]), end="")
    if (i % 10 == 9): print(" // %3d-%3d" %(i -9, i))
print("};")
