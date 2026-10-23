#!/bin/sh
# Builds out/hidkbd (static aarch64 Linux ELF, no libc) with clang + lld. The Android NDK is not needed.
#   CLANG  clang that supports --target=aarch64-linux-gnu  (default: clang in PATH)
#   LLD    ld.lld or rust-lld                              (default: ld.lld in PATH, then rustup's rust-lld)
# Local overrides can be put in build.local.sh (not tracked by git).
set -e
cd "$(dirname "$0")"
[ -f build.local.sh ] && . ./build.local.sh

CLANG=${CLANG:-$(command -v clang || command -v clang.exe || true)}
[ -n "$CLANG" ] || { echo "clang not found: install LLVM/clang or set CLANG" >&2; exit 1; }
if [ -z "$LLD" ]; then
  LLD=$(command -v ld.lld || command -v ld.lld.exe || true)
  for f in "$HOME"/.rustup/toolchains/stable-*/lib/rustlib/*/bin/rust-lld*            "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld*; do
    [ -n "$LLD" ] || { [ -f "$f" ] && LLD=$f; }
  done
fi
[ -n "$LLD" ] || { echo "lld not found: install lld (or Rust via rustup) or set LLD" >&2; exit 1; }
case "$(basename "$LLD")" in rust-lld*) LLD_FLAVOR="-flavor gnu" ;; *) LLD_FLAVOR="" ;; esac

mkdir -p out
"$CLANG" --target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-stack-protector -fno-pic \
  -O2 -Wall -c device/hidkbd.c -o out/hidkbd.o
"$LLD" $LLD_FLAVOR -static -e _start --gc-sections -o out/hidkbd out/hidkbd.o
echo "built out/hidkbd"
