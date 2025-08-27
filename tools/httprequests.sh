#!/bin/bash

while sleep .1; do wget http://192.168.68.108/hello -O /dev/null -o /dev/null; echo -n .; done


