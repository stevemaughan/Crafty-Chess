#!/usr/bin/env sh
# Build optimized, profile-guided (PGO) Crafty release executables.
#
# Produces three 64-bit Windows binaries targeting the standard x86-64
# micro-architecture levels, plus an optional 32-bit build if an i686 MinGW
# toolchain is available.  Each binary is built with -O3 + PGO: an instrumented
# build is run through Crafty's 64-position "bench" to collect a profile, then
# the final binary is recompiled with that profile.
#
# Run from the repo root:  sh scripts/build-releases.sh
#
# Tiers (64-bit):
#   x86-64-v3  "cutting edge"  AVX2 / BMI2 / FMA / popcnt   (Haswell 2013+ / Zen+)
#   x86-64-v2  "in between"    SSE4.2 / popcnt              (Nehalem 2008+)
#   x86-64     "basic"         baseline x86-64, no popcnt   (any 64-bit x86 CPU)
set -eu

VERSION="${VERSION:-25.2.1}"
ROOT="$(pwd)"
SRC="$ROOT/source"
DIST="$ROOT/dist"
# -mprefer-vector-width=128: on Win64 the ABI only guarantees 16-byte stack
# alignment, but 256-bit AVX accesses need 32 — PGO-driven auto-vectorization of
# a hot loop at x86-64-v3 emits 32-byte aligned moves and segfaults. Capping
# vector width at 128 bits avoids that (Crafty has no 256-bit-beneficial loops,
# so this costs nothing) while keeping popcnt/BMI2/lzcnt. Inert for v1/v2.
COMMON="-O3 -pipe -Wno-array-bounds -mprefer-vector-width=128 -DSYZYGY -DCPUS=64"
GCC="${GCC:-gcc}"

mkdir -p "$DIST"
cd "$SRC"

# build_pgo <output-name> <march-flags...>
build_pgo() {
  out="$1"; shift
  arch="$*"
  pdir="$SRC/.pgo-$out"
  echo "================================================================"
  echo "  Building $out   ($arch)"
  echo "================================================================"
  rm -rf "$pdir"; mkdir -p "$pdir"

  echo "  [1/3] instrumented build (-fprofile-generate)"
  $GCC $COMMON $arch -fprofile-generate="$pdir" -c crafty.c -o crafty.o
  $GCC -fprofile-generate="$pdir" crafty.o -o crafty_inst.exe -lwinmm

  echo "  [2/3] training run (bench)"
  printf 'bench\nquit\n' | ./crafty_inst.exe >/dev/null 2>&1 || true

  echo "  [3/3] optimized build (-fprofile-use)"
  $GCC $COMMON $arch -fprofile-use="$pdir" -fprofile-correction \
       -fprofile-partial-training -Wno-coverage-mismatch -c crafty.c -o crafty.o
  $GCC crafty.o -o "$DIST/$out" -lwinmm

  rm -f crafty.o crafty_inst.exe
  rm -rf "$pdir"
  echo "  -> $DIST/$out"
  echo ""
}

build_pgo "crafty-$VERSION-win-x86-64-v3.exe" -march=x86-64-v3
build_pgo "crafty-$VERSION-win-x86-64-v2.exe" -march=x86-64-v2
build_pgo "crafty-$VERSION-win-x86-64.exe"    -march=x86-64

# Optional 32-bit build, only if a 32-bit MinGW compiler is on PATH.
I686="$(command -v i686-w64-mingw32-gcc || true)"
if [ -n "$I686" ]; then
  GCC="$I686"
  build_pgo "crafty-$VERSION-win-x86-32.exe" -march=pentium4 -msse2
  GCC="gcc"
else
  echo "(skipping 32-bit: no i686-w64-mingw32-gcc on PATH)"
fi

echo "=== built binaries ==="
ls -la "$DIST"
