#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/config

mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF 
sleep 1 
esptool.py -c auto -p ${PORT} chip_id
sleep 3
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON
sleep 5


