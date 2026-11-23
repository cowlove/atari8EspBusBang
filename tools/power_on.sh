#!/bin/bash
# Power the Atari on through its Tasmota plug and wait for the plug to
# confirm it (tools/power_set.sh: retried; exit 1 if never confirmed).
exec "$(dirname "$0")/power_set.sh" ON
