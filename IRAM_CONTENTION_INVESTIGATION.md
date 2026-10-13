# ESP32-S3 IRAM contention investigation

## Purpose

This note records an A/B experiment on the ESP32-S3 Atari PBI bit-bang loop. The question was whether sustained instruction fetches by Core0 could disturb the cycle-sensitive Core1 loop when both routines execute from the same physical internal SRAM block. It is an empirical hypothesis: the public TRM material reviewed so far does not specify a precise same-bank, two-core IBUS collision penalty or its cycle granularity.

## Symptom and initial evidence

Core1 runs `iloop_pbi()` as a timing-critical GPIO bit-bang loop. Core0 handles housekeeping, including `busyWait6502Ticks(5000)` in the stress path. With the unpadded Core0 wait loop, the external hardware repeatedly failed on the first pass through the problematic section. The wait loop in the unpadded ELF compiled to a very short CCOUNT polling loop (CCOUNT read, subtraction/compare, backward conditional branch). Adding 100 NOPs to the wait made the test reliable by greatly reducing the frequency of CCOUNT polling and backward branches.

Both cores' relevant code is in IRAM; this is not an I-cache/flash explanation. The earlier ELF/map inspection found the hot routines in SRAM0 Block1. Exact IRAM bank arbitration details remain undocumented in the public references checked, so the mechanism is inferred from the controlled placement result, not claimed from a documented cycle-level rule.

## Placement experiment

In the `.4` sandbox, `iloop_pbi()` was given its own input section and mapped by `main/link.lf`. The linker rule aligns the section to a 64 KiB boundary and advances the following IRAM placement past that block. The successful ELF/map showed:

- `.literal.iloop_pbi`: `0x40390000`, 20 bytes.
- `iloop_pbi()`: `0x40390014`, 290 bytes.
- Following IRAM placement begins at `0x403a0000`.

`0x40390000` is in ESP32-S3 SRAM1 Block4. The compiler-emitted literal pool is in IRAM in the same reserved block, not flash. The inspected function disassembly contained no call instructions. Other project globals continue to rely on their explicit `DRAM_ATTR`/`IRAM_ATTR` placement; this fragment is intentionally per-function, not a whole-component placement policy.

After booting this build, Jim reported that it ran for 60 seconds and completed thousands of iterations through the previously failing section without a crash. The prior unpadded build repeatedly crashed on its first iteration. This is strong evidence that moving the Core1 loop away from the shared hot-code bank changes the failure behavior and supports the same-bank contention hypothesis. It does not yet identify whether the underlying mechanism is bank arbitration, fetch buffering/refill, or another placement-sensitive effect.

## Interpretation and caveats

- The result is compelling but not a complete proof of the precise hardware mechanism. Repeat the test for longer and, if practical, move the loop back to the original block to establish a reversible A/B/A result.
- The experiment changes linker placement and gives the function a dedicated section/noinline treatment. Keep other code and compiler settings fixed in future comparisons; compare ELF disassembly and map files, not just source.
- The 64 KiB alignment/post-alignment intentionally reserves a 64 KiB address span for isolation. Confirm the selected physical block is available and that the whole function and its literals remain inside it after every link.
- IRAM code may still have compiler-generated literal entries. Check their addresses, and inspect for calls or data references that escape the isolated block.
- The older broad `* (noflash)` mapping was abandoned. The current fragment maps only `main:iloop_pbi`; it does not guarantee that all code, globals, or data avoid flash/PSRAM.
- No cycle-level collision penalty or universal “same 32 KiB block” rule is asserted here. SRAM0 is divided into 16 KiB blocks; SRAM1 includes 64 KiB blocks. Use the ESP32-S3 TRM memory map and the actual linked addresses when describing physical placement.

## Reproducing and verifying

Build using the project's normal ESP-IDF environment and the same defines/configuration used for the hardware run. Build only; do not flash as part of map verification. Then inspect `build/atari8EspBusBang.map` and the ELF disassembly to confirm:

1. The literal pool and complete `iloop_pbi()` body are in the intended IRAM block.
2. The next IRAM placement is beyond the reserved block.
3. The loop has no unexpected calls and no references to flash/PSRAM data.
4. The Core0 wait routine and all other relevant code retain the intended control placement.

The hardware build command supplied was `idf.py -DRUNTIME_SEC=600 -DBOOT_CONFIG=\\"STRESS\\" -DTEST_HW_ID=4 --ccache build`. Quoting of `BOOT_CONFIG` was problematic in the investigative build; confirm the effective compiler define before claiming a byte-for-byte reproduction of the hardware image.

## References

- Espressif, [ESP32-S3 Technical Reference Manual](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf), especially the internal SRAM address map and SRAM access sections.
- Project ELF/map artifacts under `build/` for the specific build under test.
