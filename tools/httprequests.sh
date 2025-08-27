#!/bin/bash

while sleep .01; do wget http://192.168.68.108/hello -T 5 -O /dev/null -o /dev/null; echo -n .; done


