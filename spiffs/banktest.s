; banktest.c's cartridge probes: the first bytes of banks 2 and 3, at $A000.
; Each bank holds only its probe, so it lands at the start of the bank.
; Bank 2 (A) returns at once; bank 3 (B) counts in X first. Both banks are
; retained ("R") because nothing calls them by name.

        .section .bank2,"axR",@progbits
        .globl  cartProbeA
cartProbeA:
        rts

        .section .bank3,"axR",@progbits
        .globl  cartProbeB
cartProbeB:
        inx
        rts
