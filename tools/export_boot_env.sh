#!/usr/bin/env bash

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
config_file="${project_root}/tools/config"
_load_env_port_set="${PORT+x}"
_load_env_port="${PORT-}"
# SIDECAR_PORT likewise: a value in the environment wins over tools/config,
# so one run can aim at another board (boot.sh -sc, sidecar/flash_spiffs.sh).
_load_env_sidecar_port_set="${SIDECAR_PORT+x}"
_load_env_sidecar_port="${SIDECAR_PORT-}"

if [[ ! -e "${config_file}" ]]; then
	( umask 077; cat > "${config_file}" <<'EOF'
# Local project settings. Uncomment and edit the values you need.
# export PORT=/dev/ttyACM0
# export MQTT_HOST=192.168.1.10
# export TAS=tasmota_DEVICE
EOF
	) || return 1
fi

. "${config_file}"

if [[ -n "${_load_env_port_set}" ]]; then
	PORT="${_load_env_port}"
fi
if [[ -n "${_load_env_sidecar_port_set}" ]]; then
	export SIDECAR_PORT="${_load_env_sidecar_port}"
fi
unset _load_env_port_set _load_env_port _load_env_sidecar_port_set _load_env_sidecar_port

if [[ -z "${PORT:-}" ]]; then
	serial_devices=(/dev/ttyACM*)
	if (( ${#serial_devices[@]} == 1 )) && [[ -c "${serial_devices[0]}" ]]; then
		export PORT="${serial_devices[0]}"
		printf 'PORT is not configured; assuming the only available serial device: %s\n' "${PORT}" >&2
	fi
fi
