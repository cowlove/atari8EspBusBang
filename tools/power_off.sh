#!/bin/bash -e
cd `dirname $0`/..
. ./tools/export_boot_env.sh

if [[ -z "${MQTT_HOST:-}" || -z "${TAS:-}" ]]; then
	printf 'MQTT_HOST and TAS are unset; set them in tools/config to enable automatic Atari 8-bit system power cycling in the boot scripts. Skipping power off.\n' >&2
	exit 0
fi

power_topic="cmnd/${TAS}/POWER"
mosquitto_pub -h "${MQTT_HOST}" -t "${power_topic}" -m OFF
mosquitto_pub -h "${MQTT_HOST}" -t "${power_topic}" -m OFF
mosquitto_pub -h "${MQTT_HOST}" -t "${power_topic}" -m OFF

echo POWER OFF
