#!/bin/bash
# Build the Linux door and PUAE core, then zip a release folder with its README and FILE_ID.DIZ.
# Expects libjxl 0.11 already installed under $JXL_PREFIX (include/ and lib/). $JXL_SRC, when set,
# is the libjxl source tree, used only to copy its licence files.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=$(uname -m)
case "$ARCH" in
    x86_64|aarch64) ;;
    *) echo "This packager builds x86_64 or aarch64, not $ARCH." >&2; exit 1 ;;
esac
META="$ROOT/dist/linux-$ARCH"
JXL_PREFIX=${JXL_PREFIX:?set JXL_PREFIX to the libjxl install prefix}
WORK=$(mktemp -d)
PKG="$WORK/AmigaDoor"
trap 'rm -rf "$WORK"' EXIT

diz_ok() {
    local f=$1 line n=0
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%$'\r'}
        n=$((n + 1))
        if [ "$n" -gt 10 ] || [ "${#line}" -gt 45 ]; then
            echo "FILE_ID.DIZ must be at most 10 lines of 45 characters: $f" >&2
            printf '  %s (%s)\n' "$line" "${#line}" >&2
            exit 1
        fi
    done < "$f"
}

# Copy an ELF into the package under its soname, plus the real file when those names differ.
# Relative symlinks from the system would dangle, so the real file is what gets stored.
vendor() {
    local dep=$1 soname realdep realbase
    soname=$(basename "$dep")
    case "$soname" in
        libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*) return 0 ;;
    esac
    realdep=$(readlink -f "$dep")
    realbase=$(basename "$realdep")
    if [ ! -e "$PKG/$realbase" ]; then
        cp -a "$realdep" "$PKG/$realbase"
        echo "$PKG/$realbase" >> "$pending"
    fi
    if [ "$soname" != "$realbase" ]; then
        ln -sfn "$realbase" "$PKG/$soname"
    fi
}

diz_ok "$META/FILE_ID.DIZ"

echo "Building amigadoor ($ARCH)"
make -C "$ROOT/door" clean
make -C "$ROOT/door" -j"$(nproc)"
echo "Building puae_libretro.so"
( cd "$ROOT/door" && sh build-core.sh )

mkdir -p "$PKG/system" "$PKG/library" "$PKG/licenses/libjxl" \
    "$PKG/source/door/jxl_include" "$PKG/source/door/library" "$PKG/source/tools"
cp "$ROOT/door/amigadoor" "$ROOT/door/puae_libretro.so" "$PKG/"
cp "$META/README.txt" "$PKG/README.txt"
cp "$META/FILE_ID.DIZ" "$PKG/FILE_ID.DIZ"
cp "$ROOT/door/library/library.txt" "$PKG/library/library.txt"
cat > "$PKG/system/PUT-ROMS-HERE.txt" <<'EOF'
Put your Kickstart ROMs in this folder. Names and sizes are in README.txt under THE ROMS.
EOF

pending="$WORK/pending"
donef="$WORK/done"
: > "$pending"
: > "$donef"
echo "$PKG/amigadoor" >> "$pending"
echo "$PKG/puae_libretro.so" >> "$pending"

# libjxl is dlopened by soname from this folder. Pull every matching library out of the prefix.
while IFS= read -r dep; do
    vendor "$dep"
done < <(find "$JXL_PREFIX" \( -type f -o -type l \) \
    \( -name 'libjxl.so*' -o -name 'libjxl_cms.so*' -o -name 'libjxl_threads.so*' \
       -o -name 'libbrotli*.so*' -o -name 'libhwy.so*' -o -name 'liblcms2.so*' \))

while [ -s "$pending" ]; do
    current=$(head -n 1 "$pending")
    tail -n +2 "$pending" > "$pending.next"
    mv "$pending.next" "$pending"
    [ -e "$current" ] || continue
    real=$(readlink -f "$current")
    if grep -qxF "$real" "$donef"; then
        continue
    fi
    echo "$real" >> "$donef"
    if ! ldd "$real" >/dev/null 2>&1; then
        continue
    fi
    while read -r dep; do
        [ -n "$dep" ] || continue
        vendor "$dep"
    done < <(ldd "$real" | sed -n 's/.* => \(.*\) (.*/\1/p')
done

# $ORIGIN so a dlopened libjxl finds brotli, highway and lcms in this same folder.
while IFS= read -r f; do
    strip --strip-unneeded "$f" 2>/dev/null || true
    patchelf --set-rpath '$ORIGIN' "$f"
done < <(find "$PKG" -maxdepth 1 -type f \( -name 'amigadoor' -o -name '*.so*' \))

echo "Checking dynamic libraries"
fail=0
while IFS= read -r f; do
    if ldd "$f" | grep -q 'not found'; then
        echo "UNRESOLVED $f" >&2
        ldd "$f" >&2
        fail=1
    fi
done < <(find "$PKG" -maxdepth 1 -type f \( -name 'amigadoor' -o -name '*.so*' \))
[ "$fail" -eq 0 ]

# Source and licences travel with the binary (GPL-2).
cp "$ROOT/README.md" "$ROOT/LICENSE" "$PKG/source/"
cp "$ROOT/LICENSE" "$PKG/licenses/GPL.TXT"
cp "$ROOT/door/"*.c "$ROOT/door/"*.h "$ROOT/door/"*.py "$ROOT/door/Makefile" \
    "$ROOT/door/build-core.sh" "$ROOT/door/BUILDING-WINDOWS.txt" "$ROOT/door/INSTALL.md" \
    "$PKG/source/door/"
cp -a "$ROOT/door/jxl_include/." "$PKG/source/door/jxl_include/"
cp "$ROOT/door/library/library.txt" "$PKG/source/door/library/"
cp "$ROOT/tools/winlaunch.c" "$ROOT/tools/test_windoor.py" "$ROOT/tools/test_winupload.py" \
    "$PKG/source/tools/"
if [ -n "${JXL_SRC:-}" ] && [ -d "$JXL_SRC" ]; then
    cp "$JXL_SRC/LICENSE" "$PKG/licenses/libjxl/LICENSE.libjxl"
    [ -f "$JXL_SRC/PATENTS" ] && cp "$JXL_SRC/PATENTS" "$PKG/licenses/libjxl/PATENTS-libjxl"
    [ -f "$JXL_SRC/third_party/brotli/LICENSE" ] && cp "$JXL_SRC/third_party/brotli/LICENSE" "$PKG/licenses/libjxl/LICENSE.brotli"
    [ -f "$JXL_SRC/third_party/highway/LICENSE" ] && cp "$JXL_SRC/third_party/highway/LICENSE" "$PKG/licenses/libjxl/LICENSE.highway"
fi
if [ -f /usr/share/doc/liblcms2-2/copyright ]; then
    cp /usr/share/doc/liblcms2-2/copyright "$PKG/licenses/libjxl/LICENSE.lcms2"
fi

cp "$META/FILE_ID.DIZ" "$WORK/FILE_ID.DIZ"
cp "$META/README.txt" "$WORK/README.txt"
OUT="$ROOT/dist/AmigaDoor-Linux-$ARCH.zip"
rm -f "$OUT"
(
    cd "$WORK"
    zip -r -X "$OUT" FILE_ID.DIZ README.txt AmigaDoor
)
echo "Wrote $OUT"
