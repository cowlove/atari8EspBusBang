# The miner6 test pool: what is where

Update this table whenever hardware moves between slots (and say when). Slot N is the checkout `~/src/atari8EspBusBang.N` on miner6.local, with its own `tools/config` (untracked): the ports there (by-id, so by the ESP32's MAC) and the Tasmota plug follow whatever is cabled to that slot. After a move, check that the slot's `tools/config` still names the boards and plug now there, and that `tools/testpool_run.sh`'s line for the slot matches (`-sc` with a Sidecar, `-DBOARD_VERSION=2` for a v2 board).

Last moved: 2026-11-27 (.1 and .3 swapped). Ports and plugs read from each slot's `tools/config` on 2026-11-28.

| Slot | Atari | Main board (version) | Main MAC (`PORT`) | Sidecar MAC (`SIDECAR_PORT`) | Plug (`TAS`) | Notes |
|---|---|---|---|---|---|---|
| .1 | ? (came from .3) | v1 | FC:01:2C:2D:56:6C | none | tasmota_9575F5 | |
| .2 | 800XL | v1 | E8:F6:0A:CE:2C:B4 | none | tasmota_71D51D | loses a parked 6502 to D5 unless the park is D5-proof (AGENTS.md) |
| .3 | 800XL | v2 (carrier) | E8:F6:0A:D1:CC:B0 | 7C:4F:AD:BD:0B:FC | tasmota_955168 | same D5 behavior as .2; its Sidecar samples the bus (REBOOT_TIMELINE) |
| .4 | 130XE | v2 (carrier) | E8:F6:0A:CE:C0:74 | 98:C3:77:A1:92:14 | tasmota_955870 | |
| .5 | 130XE | v1 | FC:01:2C:2D:89:50 | none | tasmota_667941 | a history of flakiness; 2 A supply since 2026-11-18 |
| .6 | 600XL | v1, bodged (only the link wired to the Sidecar) | FC:01:2C:2D:85:58 | 68:9D:D2:C2:FE:74 | tasmota_972A4E | the Sidecar can't sample the bus; main's USB console is gone (GPIO19 is the link): reach main through the Sidecar |

Columns worth adding when known: each Atari's own identity (serial, label, mods such as a RAM upgrade or a FujiNet), its OS revision, and the power supply.
