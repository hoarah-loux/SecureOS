#!/usr/bin/env bash
#
# Rebuilds the vendored newlib in this directory from upstream source.
#
# You do not need to run this to build SecureOS. lib/ and include/ are
# committed; this script exists so the committed artifacts are reproducible
# and so the exact configuration is recorded as something executable rather
# than as prose. See README.md.
#
# Everything happens in a scratch directory that is deleted afterwards; the
# only thing this touches in the repository is lib/ and include/.

set -euo pipefail

NEWLIB_VERSION=4.4.0.20231231
NEWLIB_TARBALL="newlib-${NEWLIB_VERSION}.tar.gz"
NEWLIB_URL="https://sourceware.org/pub/newlib/${NEWLIB_TARBALL}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> scratch directory: $WORK"

# ---------------------------------------------------------------------------
# The "cross toolchain"
#
# newlib's configure insists on a compiler named after the target, but for
# x86_64-elf on an x86_64 Linux host there is nothing to cross-compile: the
# host gcc already emits exactly the objects the target wants, and the only
# real differences are flags. So the toolchain is a handful of one-line
# wrappers, and the multi-hour binutils+gcc bootstrap the usual bare-metal
# instructions call for is skipped entirely.
#
# This is also why the result is portable across the machines that build this
# repo: the artifacts are plain x86_64 SysV objects, and the libgcc they end
# up beside comes from whichever host gcc performs the final link.
# ---------------------------------------------------------------------------
XBIN="$WORK/xbin"
mkdir -p "$XBIN"

# -ffreestanding    no hosted-libc assumptions while building the libc itself
# -mno-red-zone     the kernel takes interrupts on these threads
# -fno-pie          the ELF loader performs no relocation; ET_EXEC only
# -fno-stack-protector  there is no canary and no libc to report one
cat > "$XBIN/x86_64-elf-gcc" <<'WRAPPER'
#!/usr/bin/env bash
exec gcc -ffreestanding -mno-red-zone -fno-pie -fno-stack-protector "$@"
WRAPPER

for tool in ar as ld nm objcopy objdump ranlib readelf strip size strings; do
    cat > "$XBIN/x86_64-elf-$tool" <<WRAPPER
#!/usr/bin/env bash
exec $tool "\$@"
WRAPPER
done

chmod +x "$XBIN"/x86_64-elf-*
export PATH="$XBIN:$PATH"

# ---------------------------------------------------------------------------
# Fetch and build
# ---------------------------------------------------------------------------
echo "==> fetching newlib $NEWLIB_VERSION"
curl -sSL -o "$WORK/$NEWLIB_TARBALL" "$NEWLIB_URL"
tar -xzf "$WORK/$NEWLIB_TARBALL" -C "$WORK"

SRC="$WORK/newlib-${NEWLIB_VERSION}"
BUILD="$WORK/build"
PREFIX="$WORK/install"
mkdir -p "$BUILD"
cd "$BUILD"

# --disable-newlib-supplied-syscalls
#     The syscall layer is ours: src/userland/libc_glue/syscalls.c provides
#     _read/_write/_open/_close/_lseek/_sbrk/_exit/... against this kernel's
#     int 0x80 ABI. Without this newlib links its own libgloss stubs and they
#     collide with ours.
# --disable-multilib
#     One ABI. No 32-bit or alternate-model variants.
# --enable-newlib-io-c99-formats, --enable-newlib-io-long-long
#     printf("%lld"), "%zu" and friends, which the test programs and doom use.
# --disable-newlib-atexit-dynamic-alloc
#     Keeps the atexit chain off the heap, so flushing stdio at exit does not
#     depend on malloc still being in a good state.
echo "==> configuring"
"$SRC/configure" \
    --target=x86_64-elf \
    --prefix="$PREFIX" \
    --disable-multilib \
    --disable-newlib-supplied-syscalls \
    --enable-newlib-io-c99-formats \
    --enable-newlib-io-long-long \
    --disable-newlib-atexit-dynamic-alloc \
    > "$WORK/configure.log" 2>&1

echo "==> building"
make -j"$(nproc)" > "$WORK/make.log" 2>&1
make install    > "$WORK/install.log" 2>&1

# ---------------------------------------------------------------------------
# Install into the repository
# ---------------------------------------------------------------------------
echo "==> vendoring into $HERE"
rm -rf "$HERE/lib" "$HERE/include"
mkdir -p "$HERE/lib"
cp "$PREFIX/x86_64-elf/lib/libc.a" "$HERE/lib/"
cp "$PREFIX/x86_64-elf/lib/libm.a" "$HERE/lib/"
cp -r "$PREFIX/x86_64-elf/include" "$HERE/include"

echo "==> done"
ls -la "$HERE/lib"
