#!/usr/bin/python3
import sys
import re


#arduino-cli -v compile --build-path ./build/esp32/ --clean -b esp32:esp32:esp32 --board-options PartitionScheme=min_spiffs -u -p /dev/ttyUSB0

# TODO: needs to parse out archives too 
archives = ()
objs = ()
cmds = {}
archiveDeps = {}
archiveCmds = {}
binCmd = ""

for line in map(str.rstrip, sys.stdin):
    m = re.search('-o ([^ ]+[.]((bin)|(o)|(elf)))', line)
    if m:
        if m.group(2) == "elf":
            elfCmd = line
            elf = m.group(1)
        elif m.group(2) == "bin":
            binCmd += "\t" + line + "\n"
            binFile = m.group(1)
        else:
            o = m.group(1)
            objs = objs + (o,)
            cmds[o] = line
            m = re.search(' (/[^ ]+[.]ino[.]cpp) .*/([^/]+)[.]ino[.]cpp[.]o', line)
            if m:
                sketchcpp = m.group(1)
                sketch = m.group(2)
    m = re.search('--port "(/dev/tty[^"]+)"', line)
    if m:
        port = m.group(1)
        uploadCmd = re.sub('--port "/dev/tty[^"]+"', '--port "${PORT}"', line)
    m = re.search('FQBN: esp32:esp32:([^:]+)', line)
    if m:
        board = m.group(1)
    m = re.search('-ar cr (.*[.]a) (.*[.]o)', line)
    if m:
        if archiveDeps.get(m.group(1)) == None:
            archiveDeps[m.group(1)] = ""
            archiveCmds[m.group(1)] = ""
        archiveDeps[m.group(1)] = archiveDeps[m.group(1)] + " " + m.group(2)
        archiveCmds[m.group(1)] = archiveCmds[m.group(1)] + "\t" + line + "\n"


  

print('PORT:="' + port +'"')

print(sketchcpp + ": ./" + sketch + ".ino")
print('\techo "#include <Arduino.h>" > $@')
print("\tcat ./" + sketch + ".ino > $@")

print(elf, end=": ")
for o in objs:
    print(o, end = " ")
for ar in archiveCmds:
    print(ar, end = " ")
print("\n\t@echo `basename " + elf + "`\n\t@" + elfCmd)

print(binFile + ": " + elf)
print(binCmd)

print("elf: " + elf)
print("bin: " + binFile)
print("upload: " + binFile)
print("\t" + uploadCmd)

for o in objs:
    print(o + ":\n\t@echo `basename " + o + "`\n\tccache " + re.sub("^([^ ]+) ", r"\1 ${EXTRA_CFLAGS} ", cmds[o]))

for ar in archiveCmds:
    print(ar + ": " + archiveDeps[ar])
    print("\t@echo `echo " + ar + "`\n\t" + archiveCmds[ar])

print("clean:")
for o in objs + (binFile, elf,):
    print("\t-@rm -f " + o)

for o in objs:
    print ("-include " + re.sub("[.]o$", ".d", o))

