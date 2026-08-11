/* Pad .text / .sdata so elf2dol emits 32-byte-aligned DOL section sizes.
 *
 * Must stay alive under -Wl,--gc-sections (see -u in CMakeLists).
 * Linked after other objects so padding is applied at segment ends.
 */
    .section .text, "ax", @progbits
    .balign 32
    .global __butterscotch_dol_text_align
    .type __butterscotch_dol_text_align, @function
__butterscotch_dol_text_align:
    blr

    .section .sdata, "aw", @progbits
    .balign 32
    .global __butterscotch_dol_sdata_align
    .type __butterscotch_dol_sdata_align, @object
__butterscotch_dol_sdata_align:
    .byte 0
    .balign 32
