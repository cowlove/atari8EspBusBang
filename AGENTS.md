# Repository Guidance

## Firmware Smoke Tests

- Prefer `tools/boot.sh` for routine hardware build/flash/boot tests rather than invoking `idf.py flash` directly. It records timestamped serial output under `stash/` along with the Git version and a working-tree diff, providing a useful history of boot behavior.
- If the project-root IDF build cache is stale or points at an unavailable toolchain/source path, run `idf.py fullclean` from the project root, then continue with `tools/boot.sh`. `fullclean` removes generated build state only; it leaves source files and boot logs intact.
- This reproducible history is worth the inconvenience that `boot.sh` may leave serial-monitor processes running. After each run, check for stale monitors and terminate them before starting another capture.
- The script snapshots `git diff HEAD` and `git describe`, so the diff includes both staged and unstaged changes to tracked files. Untracked file contents are not included; account for those separately when they matter to a capture.

## Hardware Test Pool

- We maintain test hardware on `miner6.local`, with working directories `~/src/atari8EspBusBang.N` where `N` is currently `1` through `6`.
- Shorthand such as “git push and fast forward miner6 machine 3” means push the current branch, then SSH to `miner6.local` and run `git pull` in `~/src/atari8EspBusBang.3`. Apply the same mapping to similar machine-number references.

## ROM core 1 boot DRAM

`startCpu1()` boots core 1 through the ROM, in DRAM that single-core IDF otherwise recycles into the heap. `main/main.cpp` reserves 0x3fceb710–0x3fceee34 (`rom_app_cpu_boot`) and, once core 1 reaches `app_cpu_main()`, returns all of it except `.data_xtos` (0x3fcedf60–0x3fcee378) to the heap: 13,068 of 14,116 bytes. This depends on how the ROM and IDF handle core 1 today, so treat it as fragile:

- **What it relies on** (measured 2026-11-06 with ESP-IDF 6.1, ESP32-S3 rev 0.2):
  - The ROM's core 1 boot writes only `_Pri_4/_Pri_5_HandlerAddress` (0x3fcee370..0x3fcee378, inside `.data_xtos`). On a cold boot and after a panic reboot, nothing else in the span changes, then or later.
  - Core 1 keeps the ROM's vectors. Faults dispatch through `.data_xtos` (`xtos_exc_handler_table` etc.), which is why it stays reserved. With it intact, a core 1 fault prints `Guru Meditation Error: Core 1 panic'ed` with the faulting PC, and core 0 then stops (no reboot), whether or not the rest is reclaimed. With those tables overwritten, the result depends on the garbage: either a core 0 "Cache error" panic or a silent core 1 death.
  - `startCpu1()` runs once per boot, and the panic hook stops core 1 instead of restarting it, so the ROM never boots core 1 again over the heap.
- **Boot-time guard:** before `startCpu1()`, the reclaimable parts are filled with 0xa5. They are reclaimed only if the pattern survives core 1's boot. Otherwise the boot prints `ROM DRAM reclaim: ... keeping the span reserved` with the changed spans. The normal boot line is `Reclaimed 13068 bytes of ROM core 1 boot DRAM`. The guard cannot see a change that makes core 1 depend on the reclaimed DRAM after boot, so rerun the probes below after any IDF, ROM-ELF or chip-revision change, or any change to `startCpu1()`, `app_cpu_init()` or the panic hook.
- **Re-verification** on the isolated bench board, with `PORT=<by-id> ./tools/boot.sh -t 40 -DFAKE_CLOCK=1 -DBOOT_CONFIG=\"BENCH\" -DROM_BOOT_PROBE=N`. Afterwards, `cmake -U ROM_BOOT_PROBE build` clears the cached define.
  - `1`: what core 1's boot changes in `.data_xtos` (snapshot diff; expect 0 bytes, because the ROM rewrites the same values) and in the reclaimed parts (expect 0), and again 3 s later. Then an 8 KB allocation should land in the reclaimed part, and the heap integrity check should pass.
  - `2`: `1`, then one `abort()`. Expect a core dump and a panic reboot, the same results again after the reboot, and DONE.
  - `3`: `1`, then a core 1 store to address 4, 5 s after core 1 starts. Expect the core 1 Guru report with PC in `app_cpu_main`. The run does not reach DONE (core 0 stops), so cap it with `timeout`, and kill any leftover `cat_until`/`cat` readers.
  - `4`: `3` without the reclaim, as the control. Its output should match `3`.
  - If the ROM layout looks different, check it: `xtensa-esp32s3-elf-objdump -h ~/.espressif/tools/esp-rom-elfs/*/esp32s3_rev0_rom.elf` (`.stack_app`, `.bss_ets` … `.bss_xtos`, `.data_xtos`) and `nm -n` for `xtos_exc_handler_table`.
- **If the guard fires or a probe disagrees:** stop reclaiming, either by dropping the `reclaimRomAppCpuBootDram()` call or by leaving the reservation alone, then re-measure. A sturdier alternative not yet tried: boot core 1 from an early `ESP_SYSTEM_INIT_FN`, before IDF recycles the ROM DRAM, and park it in an IRAM spin loop on a static stack until `setup()` releases it.

## Follow-up TODO

- **Boot-time pin hazards on the main board (bookmarked 2026-11-11; fix even though a year of running hasn't damaged anything).** Data is `Pin<38, 8>`, GPIO38–45 = D0–D7, so two of the S3's boot pins are Atari data lines:
  - **GPIO43 = D5 = U0TXD.** Three sources, worst first:
    1. IDF's console was UART0 (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, 115200, USB-Serial-JTAG secondary), so the second-stage bootloader's log and every early app printf toggled D5 on every boot. **Fixed (2026-11-11):** USB-Serial-JTAG is now the only console; bench-booted to DONE. Still to confirm on a real Atari: serial commands (`halt.sh`'s `exit 2`, read only in the PBI handler) and a panic's backtrace/core dump in a crash bundle.
    2. The ROM banner: already off. `CONFIG_BOOT_ROM_LOG_ALWAYS_OFF=y` burns `UART_PRINT_CONTROL` on each board's first boot; confirm with `espefuse.py summary`.
    3. The pad's default state: at reset GPIO43's function is U0TXD, so it may sit driven high with nothing printing, until the app reconfigures it. Measure with a weak pull-down plus a scope, or a Sidecar sampling it, across a main-board reset with no Atari attached. If it's driven, a second-stage bootloader hook that makes GPIO43 an input first thing shrinks the window to the ROM's few ms; the ROM part can't be changed in software.
  - **GPIO45 = D7 = the VDD_SPI strap.** At reset it picks the flash supply voltage; if the Atari holds D7 high while the ESP resets, the chip may select 1.8 V flash and fail to boot. **Handled (2026-11-11):** `tools/boot.sh` runs `tools/efuse_check.sh` with the Atari off, which burns `set-flash-voltage 3.3V` (irreversible) on any S3 without it, unless its PSRAM is a 1.8 V part. The bench board already had it burned at the factory; the burn path was tested on a virtual eFuse chip. Check each pool board's boot log for the `efuse_check:` lines.
  - Related, not a hazard: GPIO46 is MPD and a strap; download mode needs it low, so ROM download mode works only with the Atari powered off.

- **Hot-bus system monitor (bookmarked 2026-11-12).** Goal: PBI command 11 runs hot, so ANTIC keeps showing the monitor screen built in ESP SRAM; today each looped monitor call detaches the bus, which glitches on 64 KB machines and shows garbage on 600XL-class ones (no RAM behind the mapped region). Done (4e9549da): `sysMonitor.cpp` converted to ETL (`components/etl` submodule), its code/data in internal RAM (`SYSMON_COLD` keeps boot/cold code in flash), cart changes queued as cold work, `CONFIG_A8BB_SYSMONITOR` (about 6 KB of static internal RAM when on), and the post-build hot-safety check from `SysMonitor::pbi` (verify_core0_hot_calls.py `--closed-object --check-data`). Still to do:
  1. Dispatch `cmd == 11` in `handlePbiRequest()` before the detach (next to the Sidecar hook); take the old cold path once when `coldWorkPending()`.
  2. Put the copy-out (pbiROM + 6502 copy loop) behind a flag, off by default in hot mode; native RAM keeps the original screen, which `restoreScreen()` matches.
  3. Optionally slow the 6502's monitor loop to ~15–25 Hz. Plain `IRAM_ATTR` placement, not `CORE0_HOTSPOTS_ATTR`: the monitor is a short pass at ~50 Hz.
  4. Measure the menu items' internal-heap use; test on a 600XL with the user at the keyboard.

- Audit `Pin<n, len>` constants used by timing-critical core1 code. Inspect Xtensa assembly and linker/map output for larger mask/shift values materialized through literal pools, and verify whether any such pools can reside in flash despite IRAM function attribution. Investigate before changing the template.
