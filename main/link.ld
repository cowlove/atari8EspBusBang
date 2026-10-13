/* The LLVM-MOS Atari driver loads link.ld from its working directory. */
/* __rom_base is supplied by the Makefile for each generated image. */
ENTRY(__rom_start)
SECTIONS
{
    . = DEFINED(__rom_base) ? __rom_base : 0xD800;
    .text :
    {
        __rom_start = .;
        *(.text)
    }
}
