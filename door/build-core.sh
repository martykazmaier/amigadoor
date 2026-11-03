#!/bin/sh
# Builds the door's Amiga, puae_libretro.so, from libretro/libretro-uae (PUAE, GPL-2).
#
# PUAE carries no Commodore Kickstart ROM: without one in system/ it falls back to AROS, a free replacement. The door
# never lets it get that far (main.c checks system/ first), so the sysop puts their own Kickstarts there (INSTALL.md).
#
#   sh build-core.sh [build dir]        (default: ./libretro-uae-build; needs git, gcc, make)
#
# The commit is pinned: the door's core options (core.c) were tested against it.
set -e

COMMIT=e2e31c7
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HERE/libretro-uae-build}

if [ ! -d "$SRC/.git" ]; then
    git clone https://github.com/libretro/libretro-uae.git "$SRC"
fi
cd "$SRC"
git fetch -q origin 2>/dev/null || true
git checkout -q -f "$COMMIT"
make -j"$(nproc)"
cp puae_libretro.so "$HERE/"
echo "Built $HERE/puae_libretro.so. Now put your Kickstart ROMs in $HERE/system/ (see INSTALL.md)."
