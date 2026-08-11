# Wii linker notes

`libogc_common.ld` / `rvl.ld` here are reference copies. Runtime alignment is
enforced by `src/wii/dol_segment_align.S` (preferred over replacing `-Trvl.ld`,
which conflicts with the toolchain default script).
