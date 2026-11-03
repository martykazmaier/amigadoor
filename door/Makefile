# The Amiga door. Built on the BBS box: `make`. `make win32` cross-builds amigadoor.exe for Mystic BBS for Windows
# (32-bit; with llvm-mingw or mingw-w64: WIN32_CC=<its i686 gcc>; not tried yet for the Amiga). It needs only gcc;
# libjxl (0.11 or newer, for its lossless sizes) is loaded at run time, and its headers are in jxl_include/. The Amiga
# itself is puae_libretro.so beside the door, built from libretro/libretro-uae (build-core.sh, see INSTALL.md).
# The core uses libm without linking it (log10l): the door links libm whether it needs it or not, so it is there.
CC = gcc
CFLAGS = -Wall -Wextra -Wno-format-truncation -std=c11 -O2 -Ijxl_include
LDFLAGS = -Wl,--no-as-needed -lm -ldl

OBJ = main.o door.o apc.o jxl_enc.o core.o play.o session.o

amigadoor: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

main.o: main.c door.h apc.h jxl_enc.h play.h session.h
core.o: core.c core.h libretro.h
play.o: play.c play.h core.h apc.h jxl_enc.h libretro.h session.h
session.o: session.c session.h apc.h

# ---- Windows (Mystic BBS for Windows), 32-bit ----
WIN32_CC      ?= i686-w64-mingw32-gcc
WIN32_CFLAGS   = -Wall -Wextra -Wno-format-truncation -std=c11 -O2 -Ijxl_include -D_WIN32_WINNT=0x0601 \
                 -D__USE_MINGW_ANSI_STDIO=1
WIN32_LDFLAGS  = -static -lws2_32 -lwinmm
WIN32_SRC      = main.c door.c apc.c jxl_enc.c core.c play.c session.c win32.c telnet.c zmodem.c

amigadoor.exe: $(WIN32_SRC) *.h
	$(WIN32_CC) $(WIN32_CFLAGS) $(WIN32_SRC) -o $@ $(WIN32_LDFLAGS)

win32: amigadoor.exe

clean:
	rm -f amigadoor amigadoor.exe $(OBJ)

.PHONY: clean win32
