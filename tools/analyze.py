#!/usr/bin/python3

import numpy as np
import sys
d = np.loadtxt(sys.argv[1], dtype=int)
lastLine = None
ccount = 0
clk = 3
extsel = 2
d0 = 0
d1 = 23
rw = 20
a0 = 11
a1 = 12
a2 = 10
a3 = 13
extdec = 22
ready = 21
refresh = 1

extselTL = -1 
extselTH = -1
dTE = -1 
dTL = -1

hist = np.zeros((11,100))
lineNo = 0
maxCyc = 0
for line in d:
    if lastLine is None: lastLine = line
    if lastLine[clk] == 1 and line[clk] == 0:
        if lineNo > 2:
            #print ("%3d %3d %3d" % (extselTH, extselTL, dT))
            if (extselTH >= 0 and extselTH < 100): hist[0][extselTH] = hist[0][extselTH] + 1
            if (extselTL >= 0 and extselTL < 100): hist[1][extselTL] = hist[1][extselTL] + 1
            #if (dTE >= 0 and dTE < 100): hist[2][dTE] = hist[2][dTE] + 1
            #if (dTL >= 0 and dTL < 100): hist[3][dTL] = hist[3][dTL] + 1
            if (ccount > 0 and ccount < 100): hist[6][ccount] = hist[6][ccount] + 1
        dTE = dTL = extselTL = extselTH = -1
        if ccount > maxCyc: maxCyc = ccount
        ccount = 0
    else:
        ccount = ccount + 1
    if [lineNo > 2]:
        if lastLine[extsel] == 1 and line[extsel] == 0: extselTL = ccount
        if lastLine[extsel] == 0 and line[extsel] == 1: extselTH = ccount
        if lastLine[d0] != line[d0] or lastLine[d1] != line[d1]: 
            if line[refresh] == 0:
                hist[10][ccount] = hist[10][ccount] + 1;
            elif line[rw] == 1: 
                if line[extsel] == 0:
                    hist[2][ccount] = hist[2][ccount] + 1
                else:
                    hist[3][ccount] = hist[3][ccount] + 1
            else:
                hist[4][ccount] = hist[4][ccount] + 1
        if lastLine[a0] != line[a0] or lastLine[a1] != line[a1] or lastLine[a2] != line[a2] or lastLine[a3] != line[a3]:
            hist[5][ccount] = hist[5][ccount] + 1
        if lastLine[ready] != line[ready]: hist[7][ccount] = hist[7][ccount] + 1
        if lastLine[extdec] != line[extdec]: hist[8][ccount] = hist[8][ccount] + 1
        if lastLine[refresh] != line[refresh]: hist[9][ccount] = hist[9][ccount] + 1
    #else:
    #   if lastLine[d0] != line[d0]: hist[3][ccount] = hist[3][ccount] + 1
    #if lastLine[d0] != line[d0] and ccount < 2: print ("data change at ccount %d line %d" % (ccount, lineNo))
    lastLine = line
    lineNo = lineNo + 1

print("     EXTSL  EXTSH   DESL   DESH   DWR    ADDR    CLK  READY  EXDEC    REF   DREF")
lineNo = 0
for line in hist.T:
    if lineNo <= maxCyc or np.sum(line) > 0:
        print("%3d " % lineNo, end='')
        for c in line:
            print("%6d " % c, end='')
        print("")
        #print("%3d %6d %6d %6d %6d %6d %6d %6d" % (lineNo, line[0], line[1], line[2], line[3], line[4], line[5], line[6]))
    lineNo = lineNo + 1
sum = np.sum(hist.T, axis=0)
print("SUM ", end='')
for c in sum:
    print("%6d " % c, end ='')
print("")
#print("SUM %6d %6d %6d %6d %6d %6d %6d" % (sum[0], sum[1], sum[2], sum[3], sum[4], sum[5], sum[6]))
 
