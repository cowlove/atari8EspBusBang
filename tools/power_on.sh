#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/config

mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON 



