/* PS2 main RAM, part of the launcher image (see launcher_win32.c). A plain
 * zero-initialized global lands in .bss, so it costs no file space; the
 * runtime makes it executable. */
__attribute__((aligned(4096))) unsigned char uya_ps2ram[0x01FF8000];
