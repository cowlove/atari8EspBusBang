#!/usr/bin/python3

import numpy as np

casInhMask = 0x1;
clockMask = 0x2;
resetMask = 0x40000000
addrShift = 2
addrMask = 0xffff << addrShift;
dataShift = 22;
dataMask = 0xff << dataShift;
rw = 18
rwMask = 1 << 18
refMask = 1 << 21
rom = {}
data = np.fromfile("out.bin", dtype=np.uint32)

print("DATA %08x" % dataMask);
print("RES  %08x" %resetMask);
print("RW   %08x" %rwMask);
print("REF  %08x" %refMask);
print("CASI %08x" %casInhMask);


for i in data:
    addr = (i & addrMask) >> addrShift
    data = (i & dataMask) >> dataShift
    casI = i & casInhMask
    rwI = (i & rwMask) != 0
    resI = (i & resetMask) != 0
    refI = (i & refMask) != 0
    #if (casI == 0):
    if (data != 0xff and data != 00):
        rom[addr] = data
    print("A %08x   %04x D %02x U%d RW%d REF%d RES%d" % (i, addr, data, casI, rwI, refI, resI))

for a in sorted(rom.keys()):
    print ("R %04x %02x" % (a, rom[a]))
