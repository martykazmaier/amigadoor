# Amiga door

An Amiga 500 and an Amiga 1200 as one BBS door: PUAE (libretro-uae, as a libretro core) runs on the BBS box and
callers play it in SyncTERM, the screen sent as JPEG XL pictures. Callers load games from the sysop's library (WHDLoad
`.lha` or `.adf` floppies) or upload their own by ZMODEM, and switch machine in the F12 menu. Built the same way as the
C64 door (`../C64Door`), from which most of the code comes.

**No Kickstart ROMs are included.** Each sysop supplies their own Kickstart 1.3 and 3.1 files: `door/INSTALL.md` has
the names, sizes and where they go. PUAE itself carries no Kickstart either (its fallback is AROS, a free
replacement, which the door never uses). Note, though, that the WHDLoad helper PUAE compiles into the core (its
`retrodep/WHDLoad_files.zip.c`) contains a few AmigaOS 3.1 Workbench commands and libraries (for instance
`C:Copy`, `$VER: copy 40.1`), as upstream libretro-uae ships them; the door uses the core as built from upstream.

**Windows (Mystic BBS for Windows):** `dist/AmigaDoor-Win32.zip` is a ready-to-run 32-bit build (the door, the core as
a DLL, libjxl's DLLs; no ROMs, no games) with a `README.txt` for the sysop. `door/BUILDING-WINDOWS.txt` has how it is
built (`make win32`, PUAE with llvm-mingw) and tested (`tools/`).

Everything is in `door/`: the door itself (C, `make`), `build-core.sh` for the core, and `test_door.py`.
`spike/` is the measuring tool used to decide the design.

The door was written by JSONBourne and originally published at
https://github.com/omniphil/AmigaDoor. It is free software under the GNU General Public License, version 2 or
(at your option) any later version: see `LICENSE`. This repository keeps that licence and that attribution.
PUAE is GPL-2 too (https://github.com/libretro/libretro-uae). The libjxl headers in `door/jxl_include/` carry
their own BSD licence.
