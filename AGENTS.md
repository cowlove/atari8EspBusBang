# Repository Guidance

## Firmware Smoke Tests

- Prefer `tools/boot.sh` for routine hardware build/flash/boot tests rather than invoking `idf.py flash` directly. It records timestamped serial output under `stash/` along with the Git version and a working-tree diff, providing a useful history of boot behavior.
- If the project-root IDF build cache is stale or points at an unavailable toolchain/source path, run `idf.py fullclean` from the project root, then continue with `tools/boot.sh`. `fullclean` removes generated build state only; it leaves source files and boot logs intact.
- This reproducible history is worth the inconvenience that `boot.sh` may leave serial-monitor processes running. After each run, check for stale monitors and terminate them before starting another capture.
- The script snapshots `git diff HEAD` and `git describe`, so the diff includes both staged and unstaged changes to tracked files. Untracked file contents are not included; account for those separately when they matter to a capture.

## Hardware Test Pool

- We maintain test hardware on `miner6.local`, with working directories `~/src/atari8EspBusBang.N` where `N` is currently `1` through `6`.
- Shorthand such as “git push and fast forward miner6 machine 3” means push the current branch, then SSH to `miner6.local` and run `git pull` in `~/src/atari8EspBusBang.3`. Apply the same mapping to similar machine-number references.

## Follow-up TODO

- **HIGH PRIORITY: reclaim the ~13.8 KB of internal SRAM reserved by `rom_app_cpu_boot`** (`main/main.cpp`, commit `ddc3702`). SRAM is critical; reclaiming 10+ KB is worth real work. Background: in single-core builds IDF recycles the ROM's reserved DRAM into the heap, but `startCpu1()` boots core 1 through the ROM, which then runs on its APP CPU stack (`.stack_app` 0x3fceb710–0x3fced710) and re-initializes its data (`.bss_ets` … `.bss_xtos`, to 0x3fceee34), corrupting heap objects there (the `xTaskRemoveFromEventList` assert from `nvs_flash_init`). The fix reserves the whole span. Options, in order:
  1. Measure first: drop the reservation temporarily, fill 0x3fceb710–0x3fceee34 with a pattern just before `startCpu1()`, and print every changed span after core 1 is up. Check both a cold boot and a panic reboot. If the ROM touches only the top of `.stack_app` plus a few `xtos` words, reserve just that with margin (perhaps 2–3 KB).
  2. Full reclaim: boot core 1 from an early `ESP_SYSTEM_INIT_FN`, before IDF recycles the ROM DRAM into the heap (just before `app_main`). Park it in an IRAM spin loop on a static stack until `setup()` releases it into `app_cpu_main()`. Core 1 must then never be reset through the ROM at runtime (already true: `startCpu1()` runs once per boot, and the panic hook stops core 1 rather than restarting it).
- Audit `Pin<n, len>` constants used by timing-critical core1 code. Inspect Xtensa assembly and linker/map output for larger mask/shift values materialized through literal pools, and verify whether any such pools can reside in flash despite IRAM function attribution. Investigate before changing the template.
