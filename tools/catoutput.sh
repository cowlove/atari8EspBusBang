#!/bin/bash

cat $(ls -1tr stash/*.output | tail -$(( 1 + $1 ))  | head -1)

