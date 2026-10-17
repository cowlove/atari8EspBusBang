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

The hardware build command supplied was `idf.py -DRUNTIME_SEC=600 -DBOOT_CONFIG=\\"STRESS\\" -DHW_TYPE=\\"4\\" --ccache build`. Quoting of `BOOT_CONFIG` was problematic in the investigative build; confirm the effective compiler define before claiming a byte-for-byte reproduction of the hardware image.

## References

- Espressif, [ESP32-S3 Technical Reference Manual](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf), especially the internal SRAM address map and SRAM access sections.
- Project ELF/map artifacts under `build/` for the specific build under test.

## Reusing the unused DRAM alias

The dedicated IRAM placement occupies the start of SRAM1 Block4: a 20-byte literal pool at `0x40390000`, followed by the 290-byte `iloop_pbi()` body at `0x40390014`. The linker post-alignment keeps subsequent IRAM code at `0x403a0000`, so the remainder of Block4 is not used by code. Its DRAM alias is `0x3fca0000–0x3fcaffff`.

An initial prototype registered the Block4 DRAM-alias tail with `heap_caps_add_region()` at the beginning of `setup()`, before application allocations. That prototype calculated from `&iloop_pbi` using the ESP32-S3 IRAM-to-DRAM alias offset, plus a conservative 4 KiB guard; the end is the next block boundary, `0x3fcb0000`. For the current ELF this would add approximately 61 KiB as internal heap while retaining the loop's isolated instruction-fetch placement. Registration uses `ESP_ERROR_CHECK` and asserts the loop address remains within the expected block.

This is a build-specific ESP32-S3 memory-layout assumption. Check the ELF/map and boot-time internal-heap totals after rebuilding, then rerun the hardware stress test to learn whether ordinary heap data in the loop's physical bank is harmless in this workload. The linker still skips the bank's DRAM alias by default; runtime registration adds only the selected unused subrange and must never overlap the literal pool, function, another linker section, or an existing heap.

## Reusing the unused DRAM alias

The dedicated IRAM placement occupies the start of SRAM1 Block4: a 20-byte literal pool at `0x40390000`, followed by the 290-byte `iloop_pbi()` body at `0x40390014`. The linker post-alignment keeps subsequent IRAM code at `0x403a0000`, so the remainder of Block4 is not used by code. Its DRAM alias is `0x3fca0000–0x3fcaffff`.

An initial prototype registered the Block4 DRAM-alias tail with `heap_caps_add_region()` at the beginning of `setup()`, before application allocations. That prototype calculated from `&iloop_pbi` using the ESP32-S3 IRAM-to-DRAM alias offset, plus a conservative 4 KiB guard; the end is the next block boundary, `0x3fcb0000`. For the current ELF this would add approximately 61 KiB as internal heap while retaining the loop's isolated instruction-fetch placement. Registration uses `ESP_ERROR_CHECK` and asserts the loop address remains within the expected block.

This is a build-specific ESP32-S3 memory-layout assumption. Check the ELF/map and boot-time internal-heap totals after rebuilding, then rerun the hardware stress test to learn whether ordinary heap data in the loop's physical bank is harmless in this workload. The linker still skips the bank's DRAM alias by default; runtime registration adds only the selected unused subrange and must never overlap the literal pool, function, another linker section, or an existing heap.

### Follow-up: runtime registration failed

The first runtime `heap_caps_add_region()` prototype was withdrawn after boot testing. The decoded panic backtrace places the fault in `tlsf_create()` called by `heap_caps_add_region()`, while initializing the proposed pool at `0x3fca1028`. The message was `Cache disabled but cached memory region accessed`. Although `0x3fca0000` appears as a DIRAM alias in the SoC memory-region table, this build/runtime did not safely permit adding that alias as a heap while the corresponding SRAM bank was used for the IRAM loop. Do not use the runtime registration prototype. The current source retains the isolated IRAM linker placement but removes the heap addition; reclaiming the rest of the bank needs a different linker/SoC-supported mapping strategy.

## Linker-only packing experiment

The runtime heap-registration experiment failed during TLSF pool initialization and is no longer present in the source. A safer first recovery attempt is to keep the 64 KiB pre-alignment for `iloop_pbi()` but remove the post-alignment that had forced all subsequent IRAM into the next block. The current map keeps the literal pool at `0x40390000` and the complete loop in SRAM1 Block4 (`iloop_pbi()` begins at `0x403901ac` in this build). Other IRAM code now follows in the same block; this deliberately isolates the hot Core1 loop from Core0's hot loop, not the entire bank from all code.

This build's `_iram_end` moved from `0x403a7200` to `0x40397300`, and the `.dram0.dummy` span shortened by `0xff00` bytes. The default linker/heap layout moves `_heap_start` from `0x3fcc2ed0` to `0x3fcb2fd0`, making up to 65,280 additional bytes available to the ordinary heap without a runtime heap-add call. This is a map-based prediction; boot heap statistics and the timing stress test still need to confirm it behaves as intended. Do not flash solely on the assumption that cold IRAM sharing Block4 is harmless.

## Core0 hotspot verification modes

The hotspot layout has two available policies. The explicit-section policy puts `busyWait6502Ticks()`, `bmonWaitCycles()`, `core0Loop()`, and `handlePbiRequest()` together, then aligns `iloop_pbi()` to the next 32 KiB boundary. It gives deterministic separation but can move `_iram_end` and the heap boundary; the `.3` experiment moved `_iram_end` from `0x4038a900` to `0x4038f300`, approximately 19 KiB, because of alignment padding. The gap is smaller than the earlier broad reservation, but still consumes address span.

The current default policy deliberately uses ordinary linker placement for Core0's `IRAM_ATTR` hotspot functions and keeps only `iloop_pbi()` explicitly placed. `CORE0_HOTSPOTS_ATTR` in `main/core0_hotspots.h` selects either attribute policy; the explicit linker lines are retained as a commented alternative in `main/link.lf`. To enable deterministic mode, set `CORE0_HOTSPOTS_EXPLICIT_PLACEMENT` to `1` and replace the active `iloop_pbi()` mapping with the two documented explicit mapping lines. The post-link verifier fails if those source/linker settings do not agree. It also fails if a hotspot is missing, outside IRAM, crosses a 32 KiB bank boundary, or shares `iloop_pbi()`'s bank. Default mode preserves heap space when the normal layout happens to separate them, but is validation—not a placement guarantee: layout changes can make a future build fail. Keep the hotspot functions `noinline` so their full ranges remain individually verifiable. This check covers instruction ranges, not incidental literal/data references.
