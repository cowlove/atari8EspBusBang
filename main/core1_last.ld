/* Core 1's bus loop (iloop_pbi<>(), main/core1.cpp: .text.iloop_pbi and its
 * literal pools) goes last in IRAM, right after .iram0.text and before
 * .iram0.text_end, with no alignment padding. Core 0's hot code is IRAM_ATTR
 * (.iram1.*), which the linker places first in .iram0.text, so the loop
 * lands in a later 32 KB SRAM bank than core 0's hotspots without the
 * 8 KB ALIGN that used to pad 6-6.6 KB before it (main/link.lf).
 * tools/verify_iram_hotspots.py still checks every build: it fails if
 * IRAM shrinks enough that the last bank holds a core 0 hotspot.
 * The loop's bank is also where .data begins (DRAM follows IRAM).
 *
 * Core 1's hot data (CORE1_HOT_BSS, main/mmu.h: core1Hot, the L1 blocks,
 * d000Read/d000Write, pbiROM, the bmon tails, cartBankLatch) goes in its
 * own section after .bss, aligned to the next 32 KB SRAM bank: a bank after
 * IRAM code's last one and after core 0's interrupt stack (in .data), however
 * the sizes before it move. The gap before it goes to the heap at boot
 * (mmuHotDataGapToHeap()), and mmu.cpp zeroes the section (IDF clears
 * only .bss). tools/verify_core1_hot_data.py checks every build. */
SECTIONS
{
  .iram0.core1 :
  {
    . = ALIGN(4);
    _iloop_pbi_start = ABSOLUTE(.);
    KEEP(*libmain.a:core1.*(.literal._Z9iloop_pbiIL14CartBankSelect0EEvv .literal._Z9iloop_pbiIL14CartBankSelect1EEvv .literal.iloop_pbi .text.iloop_pbi))
    _iloop_pbi_end = ABSOLUTE(.);
  }
}
INSERT AFTER .iram0.text;

SECTIONS
{
  .dram0.core1_hot (NOLOAD) :
  {
    _core1_hot_gap_start = ABSOLUTE(.);
    . = ALIGN(0x8000);
    _core1_hot_start = ABSOLUTE(.);
    KEEP(*(.core1_hot))
    . = ALIGN(16);
    _core1_hot_end = ABSOLUTE(.);
  }
}
INSERT AFTER .dram0.bss;
