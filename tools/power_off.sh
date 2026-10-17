#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/export_boot_env.sh

if [[ -z "${MQTT_HOST:-}" || -z "${MQTT_POWER_TOPIC:-}" ]]; then
	printf 'MQTT_HOST and MQTT_POWER_TOPIC are unset; set them in tools/config to enable automatic Atari 8-bit system power cycling in the boot scripts. Skipping power off.\n' >&2
	exit 0
fi

mosquitto_pub -h "${MQTT_HOST}" -t "${MQTT_POWER_TOPIC}" -m OFF
mosquitto_pub -h "${MQTT_HOST}" -t "${MQTT_POWER_TOPIC}" -m OFF
mosquitto_pub -h "${MQTT_HOST}" -t "${MQTT_POWER_TOPIC}" -m OFF
