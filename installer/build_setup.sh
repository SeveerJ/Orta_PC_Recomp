#!/bin/sh
# Builds Setup.exe on Linux with clang + lld-link, no C runtime needed.
#   MWINC: mingw-w64 headers (mingw-w64-headers/include + crt, with _mingw.h made)
#   WLIB:  import libraries made from mingw-w64-crt's .def files (llvm-dlltool)
set -e
cd "$(dirname "$0")"
MWINC=${MWINC:-/home/claude/deps/mwinc}
WLIB=${WLIB:-/home/claude/deps/wlib}
LLVM=${LLVM:-/usr/lib/llvm-18/bin}
clang --target=x86_64-w64-mingw32 -isystem "$MWINC" -O2 -ffreestanding -fno-builtin \
      -mno-stack-arg-probe -fno-stack-protector -Wall -Wno-unused-function -c setup.c -o setup.o
$LLVM/llvm-rc /fo setup.res setup.rc
lld-link /nodefaultlib /entry:start /subsystem:windows,6.0 /out:Setup.exe setup.o setup.res \
    "$WLIB/kernel32.lib" "$WLIB/user32.lib" "$WLIB/gdi32.lib" "$WLIB/comctl32.lib" \
    "$WLIB/comdlg32.lib" "$WLIB/shell32.lib" "$WLIB/ole32.lib" "$WLIB/urlmon.lib"
rm -f setup.o setup.res
ls -la Setup.exe
