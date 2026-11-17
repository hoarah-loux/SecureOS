# Vendored newlib

**Do not edit anything in `lib/` or `include/`.** They are build output,
committed deliberately. Regenerate them with `./rebuild.sh` instead.

## What this is

`libc.a`, `libm.a` and the headers for **newlib 4.4.0.20231231**, built for
`x86_64-elf`. Userland C programs link against these; the kernel does not use
them at all and never will.

## Why it is committed rather than built

The alternative is requiring an `x86_64-elf` cross toolchain on every machine
that builds SecureOS. That means an AUR package on Arch and a from-source
build on Debian, which is where the main build runs — so a fresh clone would
be unbuildable by default on both, for a dependency that changes approximately
never.

Committing the artifacts means a clone builds with nothing installed beyond
what the kernel already needs. The libraries are plain x86_64 SysV objects, so
the *host* `gcc` links against them directly on either distribution, and the
`libgcc` they end up beside is whichever one performs the final link.

The cost is about 8 MB in the repository, paid once.

## How it was built

`rebuild.sh` is the exact procedure, and is meant to be read as documentation
as much as run. The two things worth knowing without opening it:

**There is no cross toolchain.** newlib's `configure` insists on a compiler
named `x86_64-elf-gcc`, but on an x86_64 Linux host there is nothing to
cross-compile — the host `gcc` already emits exactly the objects the target
wants. The script writes one-line wrapper scripts named after the target and
puts them on `PATH`, which turns a multi-hour binutils+gcc bootstrap into a
few seconds of shell. The wrappers add `-ffreestanding -mno-red-zone -fno-pie
-fno-stack-protector`.

**Syscalls are ours, not newlib's.** The build is configured
`--disable-newlib-supplied-syscalls`, so newlib provides no `_read`, `_write`,
`_sbrk` or friends of its own. Those live in
[`../libc_glue/syscalls.c`](../libc_glue/syscalls.c) and go through this
kernel's `int 0x80` ABI. Building without that flag produces a `libc.a` whose
libgloss stubs collide with ours at link time.

Full configure line:

```
--target=x86_64-elf
--disable-multilib
--disable-newlib-supplied-syscalls
--enable-newlib-io-c99-formats
--enable-newlib-io-long-long
--disable-newlib-atexit-dynamic-alloc
```

## How it is used

`src/boot/Makefile` compiles userland with `-isystem
src/userland/newlib/include` and links with `-L src/userland/newlib/lib -lc
-lm -lgcc`. `libgcc` comes last because newlib's 64-bit and soft-float helpers
resolve out of it.

`.gitignore` has a blanket `*.a`, with an explicit `!src/userland/newlib/lib/*.a`
exception for this directory.
