#!/usr/bin/env bash

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
config_file="${project_root}/tools/config"
_load_env_port_set="${PORT+x}"
_load_env_port="${PORT-}"

if [[ ! -e "${config_file}" ]]; then
	( umask 077; cat > "${config_file}" <<'EOF'
# Local project settings. Uncomment and edit the values you need.
# export PORT=/dev/ttyACM0
# export MQTT_HOST=192.168.1.10
# export MQTT_POWER_TOPIC=cmnd/tasmota_DEVICE/POWER
EOF
	) || return 1
fi

. "${config_file}"

if [[ -n "${_load_env_port_set}" ]]; then
	PORT="${_load_env_port}"
fi
unset _load_env_port_set _load_env_port

if [[ -z "${PORT:-}" ]]; then
	serial_devices=(/dev/ttyACM*)
	if (( ${#serial_devices[@]} == 1 )) && [[ -c "${serial_devices[0]}" ]]; then
		export PORT="${serial_devices[0]}"
		printf 'PORT is not configured; assuming the only available serial device: %s\n' "${PORT}" >&2
	fi
fi
