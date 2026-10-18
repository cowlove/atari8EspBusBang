# MegaCart support design

## Goal

Add support for Atari 8-bit MegaCart images alongside the existing AtariMax
format. MegaCart banks are 16 KiB and occupy `$8000–$BFFF` as one window. This
is distinct from AtariMax, whose 8 KiB bank is mapped at `$A000–$BFFF`.

The LLVM-MOS SDK provides an `atari8-cart-megacart` target. Initial ESP32
support now handles 128 KiB MegaCart CAR images (eight 16 KiB banks), selects
the data-based Core1 specialization at startup, and maps both 8 KiB halves of
the selected 16 KiB bank. Other MegaCart capacities are not yet supported.

## Bank protocol and mapping

MegaCart selection is triggered by a write in `$D500–$D5FF`; the written data
byte selects the bank. Bit 7 disables the cartridge, and the low seven bits
select the bank. Core1 observes only writes to `$D500`, commits the write latch,
then updates the selected bank for the next bus cycle. Requests that are
disabled or outside the eight loaded banks map the cartridge window to base
RAM. AtariMax continues using its existing address-based mapping path.

For an enabled bank, map its first 8 KiB at `$8000–$9FFF` and its second 8 KiB
at `$A000–$BFFF` as one selection. The loader should validate the CAR type and
image length on 16 KiB boundaries, and keep only as many banks resident as
internal SRAM permits. A request for an unbacked bank must safely disable or
unmap the cartridge rather than access beyond the loaded image. The supported
bank limit and behavior for short/prefix images need to be explicit; do not
silently treat an arbitrary byte-truncated CAR file as valid.

## Core1 loop structure

The current AtariMax Core1 loop is timing-critical and must retain its existing
instruction path. Do not add a runtime format test or a second bank-decode path
to that hot loop.

Use one source-level templated loop with a compile-time cartridge policy for
the bank-decode operation. Instantiate specialized AtariMax and MegaCart
implementations in IRAM, then select the correct implementation once at boot
after loading the selected configuration and cartridge. The generated code
will contain separate specialized loops, but the timing-sensitive loop body
remains maintained in one place and has no runtime format branch.

Before accepting the refactor, compare the AtariMax specialization's generated
assembly and timing against the current implementation. Confirm the existing
AtariMax decode path adds no instructions, and verify that both specializations
fit the available IRAM. Test the MegaCart write-data sampling and 16 KiB remap
separately; actual Atari hardware testing is still required for final
validation.
