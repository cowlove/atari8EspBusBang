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

## Hot bus and cold bus

- **Hot bus:** core 1's bus loop (`iloop_pbi`) is serving live 6502 bus cycles. **Hot-bus code** is core 0 code that runs in that window. It must run from IRAM and touch only internal SRAM: no flash (including `std::` and libc helpers that live there), no PSRAM, and no peripheral registers (including esp_timer/SYSTIMER; use CCOUNT). Mind which SRAM banks it shares with core 1 (`hotData.cpp` reports them). Core 0 interrupts are masked during bus traffic, so no ISR runs hot.
- **Cold bus:** the 6502 is parked in the stack-resident program and the bus is detached (`disableBus()` maps every L1 block to dummies). Core 0 may do anything here: flash, PSRAM, printf, Wi-Fi, FreeRTOS.
- Core 1's own bus loop is a separate, heavily scrutinized concern. "Hot bus" is about core 0.
- Known hot-bus entry points:
  - `core0Loop()` (main.cpp): the bmon wait loop, `core0_onBmonChange()` (and `mmuOnChange()`), and `core0_utilityTasks()`.
  - `handlePbiRequest()` (pbi.cpp), except for `handlePbiRequest2()`. That function's `ScopedInterruptEnable` calls `disableBus()` first and `enableBus()` last, so its body is cold. The stackprog wait after it runs hot again.
  - The Sidecar path: `SidecarHost::checkPbiRequestHook()`/`handlePbiRequest()` and the `Sidecar::` calls, which poll i2slink (`i2slink_hint/read/write`).
- `tools/verify_core0_hot_calls.py` checks hot-bus calls. It walks from root functions and fails on flash, unvetted IRAM, non-allowlisted ROM, unresolved indirect calls, and peripheral literals. `--cold NAME` marks a cold-bus boundary, which is accepted and not walked. POST_BUILD runs it on the Sidecar chain only. For the whole hot bus, run:
  ```
  python3 tools/verify_core0_hot_calls.py --nm xtensa-esp32s3-elf-nm --objdump xtensa-esp32s3-elf-objdump \
      --elf build/atari8EspBusBang.elf --map build/atari8EspBusBang.map \
      --root core0Loop --root SidecarHost::checkPbiRequestHook --root SidecarHost::handlePbiRequest \
      --cold handlePbiRequest2 --allow-archive libmain.a --allow-archive libsidecar_link.a
  ```
  That run still fails on known exit paths: the `exitReason` std::string assignments, the stackprog-timeout `sfmt()`, and `assert()` in `mmuOnChange()`. The core0Loop root joins POST_BUILD once those are resolved.
- `CORE0_HOTSPOTS_ATTR` and `verify_iram_hotspots.py` are about something else: IRAM bank placement for instruction fetch.
- `main` builds with `-fno-threadsafe-statics`. Otherwise a function-local static with a runtime initializer (EVERYN_TICKS' `lastTsc`) calls `__cxa_guard_acquire`/`release` in flash on its first pass.

## Follow-up TODO

- Audit `Pin<n, len>` constants used by timing-critical core1 code. Inspect Xtensa assembly and linker/map output for larger mask/shift values materialized through literal pools, and verify whether any such pools can reside in flash despite IRAM function attribution. Investigate before changing the template.
- Key injection in `core0_utilityTasks()`: the `atariRam[764] == 255` check (wait until the Atari has consumed the last key, CH = $FF) is under `#ifdef CSIM` only, to match the hardware loop that was running. It is a wanted feature for production too. Move it into the permanent code once the initial soak test of exp-core0util passes.
