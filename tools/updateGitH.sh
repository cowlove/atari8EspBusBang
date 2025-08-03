#!/bin/bash
TMP="/tmp/gitVersion.h.$$"
echo -n '#define GIT_VERSION "' > $TMP
echo -n "$(git describe --abbrev=6 --dirty --always)" >> $TMP
echo '"' >> $TMP
cmp $TMP ./main/gitVersion.h || cp $TMP ./main/gitVersion.h

