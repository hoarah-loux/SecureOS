############################################################
# SecureOS Boot Makefile
############################################################
#
# What this file is:
# - A Makefile describes build rules for the `make` build tool.
# - `make` uses timestamps and dependencies to decide what to rebuild.
#
# What this Makefile builds:
# - A freestanding kernel ELF image (ELF = Executable and Linkable Format).
# - A bootable ISO image that contains:
#   - /boot/secureos.elf (our kernel)
#   - /boot/grub/grub.cfg (GRUB configuration)
# - The ISO is booted by QEMU for testing.
#
# Boot stack notes / abbreviations used below:
# - GRUB = GRand Unified Bootloader.
# - UEFI = Unified Extensible Firmware Interface.
# - ISO = ISO 9660 filesystem image (commonly called a “.iso”).
# - QEMU = Quick Emulator (virtual machine).
# - OVMF = Open Virtual Machine Firmware (UEFI firmware for QEMU).
# - NASM = Netwide Assembler (x86 assembler).
# - CC = C Compiler.
# - LD = Linker.
#
# IMPORTANT: This file is *build system* only; it does not run on the target.

# ---------------- Paths ----------------

# Absolute path to the project root.
# - This is derived from the Makefile location so it works on any machine.
PROJECT_ROOT = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

# The boot build directory (this Makefile lives here).
BOOT_DIR = $(PROJECT_ROOT)/src/boot

# Kernel source root directory.
KERNEL_DIR = $(PROJECT_ROOT)/src/kernel

# SecureOS source root directory.
SRC_DIR = $(PROJECT_ROOT)/src

# Where we place intermediate build artifacts (object files, staged ISO tree).
BUILD_DIR = $(BOOT_DIR)/build

# Staging directory that becomes the ISO filesystem tree.
# - We copy the kernel and grub.cfg here before calling grub-mkrescue.
ISO_DIR = $(BUILD_DIR)/isodir

INCLUDE_DIR = $(PROJECT_ROOT)/hdr

# ---------------- Toolchain programs ----------------

# Assembler (ASM) program.
# - We use NASM (Netwide Assembler) to assemble the Multiboot2 entry stub.
ASM = nasm

# C compiler (CC) program.
# - We use GCC (GNU Compiler Collection) to compile the freestanding kernel C.
CC = gcc

# Linker (LD) program.
# - We use GNU ld to link the final kernel ELF image with our linker script.
LD = ld

# GNU assembler, used for the GAS-syntax userland test binaries at the
# project root (elf.S / elf_ring3_test.S) - distinct from $(ASM)/NASM,
# which only handles the kernel's own .asm sources.
AS = as

# GRUB ISO builder.
# - grub-mkrescue creates a bootable ISO image containing GRUB and our files.
GRUB_MKRESCUE = grub-mkrescue

# ---------------- GRUB platform selection (UEFI ISO) ----------------

# Directory containing GRUB UEFI modules for x86_64.
# - `?=` means “set this only if it is not already set by the environment”.
# - This lets you override it like:
#     make GRUB_PLATFORM_DIR=/some/other/path
GRUB_PLATFORM_DIR ?= /usr/lib/grub/x86_64-efi

# Private, patched copy of the GRUB platform directory (see below).
# - grub-mkrescue is pointed at this copy instead of the system directory, so
#   nothing outside the build tree is ever modified.
GRUB_MODS_DIR = $(BUILD_DIR)/grub-x86_64-efi

# Flags passed to grub-mkrescue.
# - `-d <dir>` tells grub-mkrescue where to find the platform modules.
GRUB_MKRESCUE_FLAGS ?= -d $(GRUB_MODS_DIR)

# NOTE: the rule that produces $(GRUB_MODS_DIR) lives further down, next to
# the ISO rule. It must not appear before the `all:` target, because make
# treats the first target in the file as the default goal - putting it up here
# makes a bare `make` build only that module and silently skip the ISO.

# ---------------- GRUB relocator workaround (UEFI + GRUB >= 2.13) ----------
#
# Symptom this exists to prevent:
#   Under UEFI, immediately after GRUB prints "Booting `SecureOS'", the
#   firmware traps with:
#     !!!! X64 Exception Type - 0E(#PF - Page-Fault) !!!!
#     ExceptionData - 0000000000000003  I:0 R:0 U:0 W:1 P:1
#   i.e. a *write* to a page that is present but read-only. The faulting
#   address is inside GRUB's own relocator module, not inside this kernel.
#
# Why it happens:
#   Multiboot2 requires the kernel to be entered in 32-bit protected mode, so
#   GRUB gets there via grub_relocator32_boot(). That function stores the
#   register values the kernel should start with directly into the `imm32`
#   operands of a `mov eax/ebx/ecx/edx, $0` trampoline template that lives in
#   the relocator module's own .text, and only then copies the patched
#   trampoline to its final location. In other words, the relocator is
#   self-modifying code.
#
#   GRUB commit d72208423dca ("apply read-only attributes according to ELF
#   section flags to loaded modules") made GRUB mark a module's .text
#   read-only via EFI_MEMORY_ATTRIBUTE_PROTOCOL, but only when the firmware
#   actually offers that protocol. Once .text is read-only, the relocator's
#   in-place patching faults - which is why this reproduces on a machine with
#   new GRUB + new EDK2/OVMF and not on one with older firmware that never
#   exposes the protocol. It is a GRUB bug, unrelated to this kernel: a
#   67-byte Multiboot2 stub that only writes one character to the serial port
#   reproduces it identically.
#
# What the fix does:
#   Upstream's fix moves the runtime-patched stubs into a section flagged
#   "awx" (alloc + write + execute) so GRUB maps them writable. This applies
#   the same change to the already-built module: copy the platform directory
#   into the build tree and add the SHF_WRITE bit to relocator.mod's .text
#   (readelf shows the flags go from `AX` to `WAX`). The system GRUB install
#   is left untouched.
#
#   This is safe on unaffected machines: without the memory attribute
#   protocol GRUB never applies the restriction, so the extra flag is inert.
#   Remove this rule once the distro ships a GRUB carrying the upstream fix.
#
#   (The recipe itself is defined further down, after the `all:` target.)

# ---------------- Build flags ----------------

# Assembler flags.
# - `-f elf64` tells NASM to emit a 64-bit ELF object file (.o).
ASMFLAGS = -f elf64

# C compiler flags.
# - `-ffreestanding`:
#     Compile for a freestanding environment (no hosted OS assumptions).
# - `-O0`:
#     Disable optimizations (best for debugging and deterministic behavior).
# - `-nostdlib`:
#     Do not link in the host system standard libraries/start files.
# - `-m64`:
#     Compile for x86_64.
# - `-mno-red-zone`:
#     Disable the x86_64 “red zone” (interrupts/exceptions could clobber it).
# - `-mno-mmx -mno-sse -mno-sse2`:
#     Avoid using MMX/SSE registers/instructions (no FPU/SIMD init yet).
# - `-fpie`:
#     Position-independent code. NOT for relocatability (we still link at a
#     fixed VA, LDFLAGS below has no -pie/-shared, so ld resolves everything
#     to concrete values at link time - no runtime relocator exists or is
#     needed). This is here because the kernel runs the *same physical
#     image* through multiple valid VA aliases at once (its low-identity
#     boot address, the physmap alias, and its higher-half alias -
#     see kmain_highhalf() in kernel.c) - -fno-pic (the previous setting)
#     bakes in one fixed absolute address for every global/static symbol
#     reference, which is only correct under whichever single alias was
#     "live" when that address was captured. -fpie makes every such
#     reference RIP-relative instead, so it resolves correctly no matter
#     which alias the referencing code is currently executing from.
#     Previously used `-mcmodel=kernel`, which is mutually exclusive with
#     -fpic/-fpie at the compiler level (hard error) - dropped in favor of
#     the default (small) code model, which is also the architecturally
#     correct one for an image actually linked in the low 2GB (1MiB),
#     unlike -mcmodel=kernel which assumes the negative/high 2GB.
# - `-fvisibility=hidden`:
#     Under -fpie, taking a *function's* address as a value (not calling
#     it - e.g. storing it in syscall_table[], or passing it to
#     set_idt_entry()) still goes through a GOT slot by default, unless
#     the compiler knows the symbol can't be interposed from outside this
#     link unit. A GOT slot holds a fixed value the static linker writes
#     once - reading it does NOT track whichever alias the reading code
#     is currently executing from, unlike a direct RIP-relative
#     computation, so it silently reintroduces the exact bug -fpie was
#     added to fix, just for function pointers instead of data. There is
#     no shared library boundary here (one fully static link, verified via
#     `readelf -d`: no dynamic section, no relocations), so nothing is
#     genuinely interposable - hidden visibility tells the compiler that
#     and it emits a direct RIP-relative load instead. Symbols defined in
#     hand-written assembly (the ISR entry points in idt.c's extern
#     declarations) aren't touched by this C compiler flag on their
#     definition side, so those get the same attribute explicitly at each
#     extern declaration instead.
# - `-fno-stack-protector`:
#     GCC on most distributions defaults to -fstack-protector-strong, which
#     emits a canary check calling `__stack_chk_fail` on function return.
#     That symbol lives in libc, which a freestanding kernel does not link,
#     so every guarded function becomes an undefined reference at link time.
#     There is also no thread-pointer set up to hold the canary this early.
# - `-fno-builtin`:
#     Do not assume the standard library semantics of functions named like
#     libc ones, and do not synthesize calls to them from recognized code
#     patterns (e.g. rewriting a byte-copy loop into a memcpy call - which in
#     src/kernel/core/string.c would make memcpy call itself). Explicit
#     struct-assignment lowering to memcpy/memset still happens regardless of
#     this flag; src/kernel/core/string.c provides those symbols out of line.
# - `-Wall -Wextra`:
#     Enable common and extra warnings.
# - `$(CFLAGS_EXTRA)`:
#     Empty by default; a hook for one-off defines from the command line
#     without editing this file. The bring-up tests use it for SHELL_AUTORUN,
#     which makes the shell run one command at startup so a headless QEMU run
#     can launch a binary with no keyboard attached:
#       make CFLAGS_EXTRA='-DSHELL_AUTORUN="\"./RING3M.EXE\""'
CFLAGS_EXTRA ?=

CFLAGS = -ffreestanding -O0 -g3 -nostdlib -m64 -mno-red-zone \
                -mno-mmx -mno-sse -mno-sse2 -fpie -fvisibility=hidden \
                -fno-stack-protector -fno-builtin -Wall -Wextra \
                -I$(INCLUDE_DIR) $(CFLAGS_EXTRA)

# Linker flags.
# - `-T <script>`:
#     Use our custom linker script to control section placement/segments.
# - `-nostdlib`:
#     Do not link host standard libraries/start files.
LDFLAGS = -T $(BOOT_DIR)/grub/linker.ld -nostdlib

# ---------------- Source files ----------------

# Assembly entry stub source.
# - This contains the Multiboot2 header and the mode/paging setup.
ASM_SOURCES = \
       $(BOOT_DIR)/grub/entry64_mb2.asm \
       $(SRC_DIR)/arch/x86_64/interrupts.asm \
       $(SRC_DIR)/arch/x86_64/context_switch.asm

# Kernel C sources.
# - Add files here as you split the kernel into modules.
# - $(wildcard ...) entries are optional and won't error if the file doesn't exist yet.
KERNEL_SRCS = \
       $(KERNEL_DIR)/core/kernel.c \
       $(KERNEL_DIR)/core/framebuffer.c \
       $(KERNEL_DIR)/core/multiboot2_utils.c \
       $(KERNEL_DIR)/core/console.c \
       $(KERNEL_DIR)/core/serial.c \
       $(KERNEL_DIR)/core/shell.c \
       $(KERNEL_DIR)/core/string.c \
       $(KERNEL_DIR)/interrupts/handlers.c \
       $(KERNEL_DIR)/interrupts/keyboard.c \
       $(KERNEL_DIR)/interrupts/pic.c \
       $(KERNEL_DIR)/interrupts/syscall.c \
       $(KERNEL_DIR)/mm/alloc/pmm.c \
       $(KERNEL_DIR)/mm/alloc/buddy.c \
       $(KERNEL_DIR)/mm/alloc/slab.c \
       $(KERNEL_DIR)/mm/alloc/heap.c \
       $(KERNEL_DIR)/mm/alloc/vmm.c \
       $(KERNEL_DIR)/fs/ide_pio.c \
       $(KERNEL_DIR)/fs/fat.c \
       $(KERNEL_DIR)/fs/fat32_cluster.c \
       $(KERNEL_DIR)/fs/fat32_dir.c \
       $(KERNEL_DIR)/fs/fat32_fs.c \
       $(KERNEL_DIR)/fs/block_device.c \
       $(KERNEL_DIR)/fs/tar.c \
       $(KERNEL_DIR)/fs/fs_syscalls.c \
       $(KERNEL_DIR)/proc/loader/loader.c \
       $(KERNEL_DIR)/proc/init_proc.c \
       $(KERNEL_DIR)/proc/ds/p.c \
       $(KERNEL_DIR)/proc/ds/t.c \
       $(KERNEL_DIR)/proc/sched/scheduler.c \
       $(KERNEL_DIR)/proc/sched/queues.c \
       $(KERNEL_DIR)/proc/sched/context.c

ARCH_C_SRCS = \
          $(SRC_DIR)/arch/x86_64/idt.c \
          $(SRC_DIR)/arch/x86_64/arch_tables.c \
          $(SRC_DIR)/arch/x86_64/cpu_sse.c

# Linker script.
LINKER_SCRIPT = $(BOOT_DIR)/grub/linker.ld

# GRUB configuration file.
GRUB_CFG = $(BOOT_DIR)/grub/grub.cfg

# ---------------- Object files (intermediate build artifacts) ----------------

# Object file produced by assembling the entry stub.
ASM_OBJS = \
       $(BUILD_DIR)/entry64_mb2.o \
       $(BUILD_DIR)/arch/x86_64/interrupts.o \
       $(BUILD_DIR)/arch/x86_64/context_switch.o

# Object files produced by compiling kernel C sources.
# - We mirror the src/kernel directory structure under the build directory.
KERNEL_OBJS = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_SRCS))

ARCH_C_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(ARCH_C_SRCS))

# ---------------- Outputs ----------------

# Final linked kernel image (ELF) placed in the build directory.
KERNEL_ELF = $(BUILD_DIR)/secureos.elf

# ---------------- FAT32 disk image ----------------

# Path to the FAT32 disk image build artifact.
FAT32_IMAGE = $(BUILD_DIR)/secureos.fat32.img

# QEMU drive string for the FAT32 image.
FAT32_DRIVE = -drive file=$(FAT32_IMAGE),format=raw,if=ide,index=0,media=disk

# Path to the final FAT32 image that will be placed in the ISO.
FAT32_FILE = $(ISO_DIR)/boot/secureos.fat32.img

# Staging directory for files that will be copied into the FAT32 image.
FAT32_ROOT = $(BUILD_DIR)/fat32_root

# Size of the FAT32 image in bytes.
FAT32_IMAGE_BYTES = 67108864

# Image label used by mkfs.fat.
FAT32_VOLUME_LABEL = SECUREOS

# Tool used to format the raw image.
MKFS_FAT = mkfs.fat

# mtools helpers for populating the image.
MCOPY = mcopy
MMD = mmd

# Final bootable ISO image placed at project root.
# - The name includes “uefi” to emphasize it’s built for UEFI GRUB.
ISO_FILE = $(PROJECT_ROOT)/secureos-uefi.iso

# ---------------- Default target ----------------

# The default target is what runs when you type just `make`.
# - It depends on the ISO file, so building the ISO is the default behavior.
all: $(ISO_FILE)

# Create the build directory if it does not exist.
# - `mkdir -p` creates parent directories as needed and is not an error if it
#   already exists.
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Assemble the entry stub into an object file.
# Target:
# - $(ASM_OBJ) is the output.
# Dependencies:
# - $(ASM_SRC) means rebuild if the assembly source changes.
# - `| $(BUILD_DIR)` is an *order-only* dependency: ensure the build directory
#   exists before running the recipe, but do not rebuild just because the
#   directory timestamp changes.

$(BUILD_DIR)/entry64_mb2.o: $(BOOT_DIR)/grub/entry64_mb2.asm | $(BUILD_DIR)
	# Run NASM with the chosen format flags and write the .o output.
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD_DIR)/arch/x86_64/%.o: $(SRC_DIR)/arch/x86_64/%.asm | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

# Compile the kernel C source into an object file.
# - `-c` means “compile only”, producing a .o without linking.
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Link the final kernel ELF.
# - We invoke the linker directly (ld) so we fully control the link.
# - The linker script defines the memory layout, segments, and symbols.
$(KERNEL_ELF): $(ASM_OBJS) $(KERNEL_OBJS) $(ARCH_C_OBJS) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) $(ASM_OBJS) $(KERNEL_OBJS) $(ARCH_C_OBJS) -o $(KERNEL_ELF)

# Build the patched private copy of the GRUB platform directory.
# - See the long "GRUB relocator workaround" comment near the top of this file
#   for why relocator.mod's .text has to be made writable.
# - The whole platform directory is copied so grub-mkrescue finds every module
#   it needs in one place; only relocator.mod is modified.
$(GRUB_MODS_DIR)/relocator.mod: $(GRUB_PLATFORM_DIR)/relocator.mod | $(BUILD_DIR)
	rm -rf $(GRUB_MODS_DIR)
	mkdir -p $(GRUB_MODS_DIR)
	cp -a $(GRUB_PLATFORM_DIR)/. $(GRUB_MODS_DIR)/
	objcopy --set-section-flags .text=alloc,load,code,contents \
		$(GRUB_MODS_DIR)/relocator.mod

# Create the bootable ISO image.
# - grub-mkrescue collects GRUB modules and our staged tree and emits an ISO.
$(ISO_FILE): $(KERNEL_ELF) $(GRUB_CFG) $(GRUB_MODS_DIR)/relocator.mod
	# Create directories inside the staged ISO tree.
	mkdir -p $(ISO_DIR)/boot/grub
	# Copy the built kernel to the path referenced by grub.cfg.
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/secureos.elf
	# Copy the GRUB configuration file.
	cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) $(GRUB_MKRESCUE_FLAGS) -o $(ISO_FILE) $(ISO_DIR)
	@echo "=== Build Complete ==="
	@echo "Bootable ISO created: $(ISO_FILE)"
	@echo "Test with: qemu-system-x86_64 -cdrom $(ISO_FILE)"

# Remove build artifacts.
# - `rm -rf` removes the build directory recursively (force, no prompt).
# - `rm -f` removes the ISO file if it exists.
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(ISO_FILE)
	@echo "Cleaned build artifacts"

# Run the built ISO in QEMU (default firmware).
# - This is convenient, but for UEFI you typically want the run-uefi targets.
run: $(ISO_FILE) $(FAT32_IMAGE)
	qemu-system-x86_64 -cdrom $(ISO_FILE) $(FAT32_DRIVE) -m 2048

# ---------------- QEMU UEFI (OVMF) run targets ----------------

# UEFI firmware code image.
# - Typically read-only, contains the firmware executable code.
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd

# UEFI firmware variables image.
# - Contains NVRAM-like variables (boot order, settings) and is writable.
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

# We make a writable copy of the variables image into the build dir so we do
# not modify the system-wide template.
OVMF_VARS_COPY = $(BUILD_DIR)/OVMF_VARS_4M.fd

# Create a per-build writable copy of the OVMF variables file.
$(OVMF_VARS_COPY): | $(BUILD_DIR)
	cp $(OVMF_VARS) $(OVMF_VARS_COPY)

# Run the ISO in QEMU using UEFI firmware (OVMF).
# QEMU options explained:
# - `-machine pc` selects the classic PC chipset model with legacy IDE.
# - `-drive if=pflash,...` attaches a “persistent flash” device for UEFI.
#   - First pflash drive is the read-only firmware code.
#   - Second pflash drive is the writable variables store.
# - `-cdrom` attaches the ISO as a virtual CD/DVD drive.
run-uefi: $(ISO_FILE) $(OVMF_VARS_COPY) $(FAT32_IMAGE)
	qemu-system-x86_64 \
		-machine pc \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_COPY) \
		-boot order=d \
		-cdrom $(ISO_FILE) \
		$(FAT32_DRIVE) \
		-m 2048

# Run the UEFI boot with serial redirected to the host terminal.
# - `-serial stdio` routes the guest COM1 serial port to QEMU standard I/O.
# - This is the most reliable early debug channel under UEFI.
run-uefi-serial: $(ISO_FILE) $(OVMF_VARS_COPY) $(FAT32_IMAGE)
	qemu-system-x86_64 \
		-machine pc \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_COPY) \
		-boot order=d \
		-cdrom $(ISO_FILE) \
		$(FAT32_DRIVE) \
		-serial stdio \
		-vga std \
		-m 2048

# Force debug-friendly compile flags for this target and its prerequisites.
run-debug: $(ISO_FILE) $(OVMF_VARS_COPY) $(FAT32_IMAGE)
	cp $(OVMF_VARS) $(OVMF_VARS_COPY)
	qemu-system-x86_64 \
		-machine pc \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_COPY) \
		-boot order=d \
		-cdrom $(ISO_FILE) \
		$(FAT32_DRIVE) \
		-serial stdio \
		-m 2048 \
		-s -S


# ---------------- Userland test binaries ----------------

# This-OS-native ELF test binary (int 0x80 SYS_write, not Linux syscall) -
# used to verify the ELF loader end-to-end. Built the same way elf.elf
# presumably was (no prior rule existed for either).
RING3_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_test.elf

$(RING3_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_test.S $(PROJECT_ROOT)/elf_ring3_test.ld | $(BUILD_DIR)
	$(AS) --64 $(PROJECT_ROOT)/elf_ring3_test.S -o $(BUILD_DIR)/elf_ring3_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_test.ld $(BUILD_DIR)/elf_ring3_test.o -o $@

# Second ring3 test binary. elf_ring3_test.S never touches its stack, so it
# passes whether or not the user stack is usable; this one writes to the stack
# at every step (push/pop, a call frame with locals, and a frame deep enough to
# demand-fault a second page) and prints which step failed, if any.
# Lands on the FAT32 image as /RING3S.EXE - see the mcopy below.
RING3_STACK_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_stack_test.elf

$(RING3_STACK_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_stack_test.S $(PROJECT_ROOT)/elf_ring3_stack_test.ld | $(BUILD_DIR)
	$(AS) --64 $(PROJECT_ROOT)/elf_ring3_stack_test.S -o $(BUILD_DIR)/elf_ring3_stack_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_stack_test.ld $(BUILD_DIR)/elf_ring3_stack_test.o -o $@

# Third ring3 test binary, for demand paging. The other two link to a single
# one-page read+exec PT_LOAD with no .bss, so they run identically whether the
# loader populates segments eagerly or lazily. This one links to a three-page
# text segment plus a separate writable segment whose .bss both shares a page
# with .data and runs several pages past it, which is what actually
# distinguishes a correct demand-paged load from a broken one.
# Lands on the FAT32 image as /RING3M.EXE - see the mcopy below.
RING3_MMAP_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_mmap_test.elf

$(RING3_MMAP_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_mmap_test.S $(PROJECT_ROOT)/elf_ring3_mmap_test.ld | $(BUILD_DIR)
	$(AS) --64 $(PROJECT_ROOT)/elf_ring3_mmap_test.S -o $(BUILD_DIR)/elf_ring3_mmap_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_mmap_test.ld $(BUILD_DIR)/elf_ring3_mmap_test.o -o $@

# Fourth ring3 test binary, the B side of the preemption test. Loaded together
# with RING3.EXE as two separate processes: both busy-loop and print, neither
# yields, so interleaved output means the timer preempted. Both then SYS_exit,
# which drains the ready queue and exercises the idle-thread fallback.
# Lands on the FAT32 image as /RING3P.EXE - see the mcopy below.
RING3_PREEMPT_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_preempt_test.elf

$(RING3_PREEMPT_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_preempt_test.S $(PROJECT_ROOT)/elf_ring3_preempt_test.ld | $(BUILD_DIR)
	$(AS) --64 $(PROJECT_ROOT)/elf_ring3_preempt_test.S -o $(BUILD_DIR)/elf_ring3_preempt_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_preempt_test.ld $(BUILD_DIR)/elf_ring3_preempt_test.o -o $@

# Fifth ring3 test binary, the only one bigger than 4 KiB. Its .pages payload
# is 20 KiB of real file content with a marker every 4096 bytes, one qword
# straddling a page boundary, and a sentinel in the final byte - so a load that
# stops at one page, walks the FAT chain wrong, or truncates the last partial
# cluster fails at a named step instead of crashing vaguely. Also the
# reproducer for TODO.md's "load_file() will fail on any file over 4KB"
# (loader.c kmallocs file_size; kmalloc() caps at 4096 in heap.c).
# Lands on the FAT32 image as /RING3B.EXE - see the mcopy below.
RING3_BIG_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_big_test.elf

$(RING3_BIG_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_big_test.S $(PROJECT_ROOT)/elf_ring3_big_test.ld | $(BUILD_DIR)
	$(AS) --64 $(PROJECT_ROOT)/elf_ring3_big_test.S -o $(BUILD_DIR)/elf_ring3_big_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_big_test.ld $(BUILD_DIR)/elf_ring3_big_test.o -o $@

# Sixth ring3 test binary, and the only one that touches a vector register.
# Checks that SSE executes at all (CR0.EM clear, CR4.OSFXSR set), that
# floating-point arithmetic is correct, and - the part nothing else covers -
# that xmm0-xmm7 survive being preempted by IRQ0, which is what the
# fxsave/fxrstor pair in save_trap_frame()/restore_context exists for.
# Lands on the FAT32 image as /RING3F.EXE - see the mcopy below.
RING3_SSE_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_sse_test.elf

# Two binaries from one source, differing only in SEED_TAG - which is the
# whole point. Step 4 checks that xmm0-7 survive preemption, and with a single
# SSE process on the system it would pass even if the context switch saved no
# vector state whatsoever: nothing else on the ready queue touches a vector
# register, because the kernel is built -mno-sse and the shell is kernel code.
# Run both together (see SHELL_AUTORUN) and they clobber each other's xmm
# registers unless fxsave/fxrstor genuinely work.
$(RING3_SSE_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_sse_test.S $(PROJECT_ROOT)/elf_ring3_sse_test.ld | $(BUILD_DIR)
	$(AS) --64 --defsym SEED_TAG=0 $(PROJECT_ROOT)/elf_ring3_sse_test.S -o $(BUILD_DIR)/elf_ring3_sse_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_sse_test.ld $(BUILD_DIR)/elf_ring3_sse_test.o -o $@

RING3_SSE2_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_sse2_test.elf

$(RING3_SSE2_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_sse_test.S $(PROJECT_ROOT)/elf_ring3_sse_test.ld | $(BUILD_DIR)
	$(AS) --64 --defsym SEED_TAG=1 $(PROJECT_ROOT)/elf_ring3_sse_test.S -o $(BUILD_DIR)/elf_ring3_sse2_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_sse_test.ld $(BUILD_DIR)/elf_ring3_sse2_test.o -o $@

# Seventh ring3 test binary: SYS_brk, SYS_gettime and SYS_sleep, all three of
# which were stubs returning -1 until now. They sit directly under newlib -
# _sbrk(), _gettimeofday(), and doomgeneric's DG_SleepMs - so checking them
# from assembly first keeps a kernel bug distinguishable from a libc
# retargeting bug. Lands on the FAT32 image as /RING3T.EXE.
RING3_TIME_TEST_ELF = $(PROJECT_ROOT)/elf_ring3_time_test.elf

$(RING3_TIME_TEST_ELF): $(PROJECT_ROOT)/elf_ring3_time_test.S $(PROJECT_ROOT)/elf_ring3_sse_test.ld | $(BUILD_DIR)
	$(AS) --64 $(PROJECT_ROOT)/elf_ring3_time_test.S -o $(BUILD_DIR)/elf_ring3_time_test.o
	$(LD) -T $(PROJECT_ROOT)/elf_ring3_sse_test.ld $(BUILD_DIR)/elf_ring3_time_test.o -o $@

# ---------------- Userland C binaries ----------------
#
# Distinct from the hand-written .S tests above: these are real C programs
# linked against newlib, and they are what the doom port is built on.
#
# The compiler is the *host* gcc, the same one that builds the kernel - not a
# cross compiler. The only thing a cross toolchain was ever needed for is
# building newlib itself, and that output is vendored into the repo (see
# USER_NEWLIB_DIR below), so a fresh clone needs nothing installed beyond what
# the kernel already requires. That matters because the main build runs on
# Debian, which has no x86_64-elf-gcc package.
#
# Flags that are not optional:
#   -mno-red-zone   the kernel takes interrupts on these threads
#   -fno-pie        the loader performs no relocation; ET_EXEC only
#   -O2             unlike the kernel's -O0; doom needs it
#   -z max-page-size=0x1000
#                   ld defaults to 2 MiB segment alignment for x86-64, which
#                   breaks the p_offset == p_vaddr (mod 4096) congruence that
#                   loader.c enforces and demand paging depends on
USER_CC = $(CC)

# Vendored newlib: libc.a / libm.a / headers, built once by a throwaway
# x86_64-elf toolchain and committed. See src/userland/newlib/README.md.
USER_NEWLIB_DIR = $(SRC_DIR)/userland/newlib

USER_SRC_DIR = $(SRC_DIR)/userland
USER_BUILD_DIR = $(BUILD_DIR)/userland
USER_LINKER = $(USER_SRC_DIR)/user.ld

USER_CFLAGS = -ffreestanding -nostdlib -O2 -g -m64 -mno-red-zone \
              -fno-pie -fno-stack-protector -fno-builtin \
              -Wall -Wextra \
              -isystem $(USER_NEWLIB_DIR)/include \
              -I$(INCLUDE_DIR) -I$(USER_SRC_DIR)/libc_glue

USER_LDFLAGS = -T $(USER_LINKER) -nostdlib -no-pie -z max-page-size=0x1000 \
               -L$(USER_NEWLIB_DIR)/lib

# libgcc last: newlib's soft-float and 64-bit helpers resolve out of it.
USER_LIBS = -lc -lm -lgcc

# Objects every userland C binary links against.
USER_CRT_OBJS = $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls.o

$(USER_BUILD_DIR):
	@mkdir -p $@

$(USER_BUILD_DIR)/crt0.o: $(USER_SRC_DIR)/crt0.S | $(USER_BUILD_DIR)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/syscalls.o: $(USER_SRC_DIR)/libc_glue/syscalls.c | $(USER_BUILD_DIR)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

# Milestone 1 of the doom port: the first C program this OS runs.
# Lands on the FAT32 image as /HELLO.EXE - see the mcopy below.
HELLO_ELF = $(USER_BUILD_DIR)/hello.elf

$(HELLO_ELF): $(USER_SRC_DIR)/hello/hello.c $(USER_CRT_OBJS) $(USER_LINKER) | $(USER_BUILD_DIR)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $(USER_BUILD_DIR)/hello.o
	$(USER_CC) $(USER_LDFLAGS) $(USER_CRT_OBJS) $(USER_BUILD_DIR)/hello.o \
	$(USER_LIBS) -o $@

.PHONY: userland
userland: $(HELLO_ELF)

# ---------------- FAT32 image build ----------------

# Rule to build the FAT32 image.
$(FAT32_IMAGE): $(RING3_TEST_ELF) $(RING3_STACK_TEST_ELF) $(RING3_MMAP_TEST_ELF) $(RING3_PREEMPT_TEST_ELF) \
		$(RING3_BIG_TEST_ELF) $(RING3_SSE_TEST_ELF) $(RING3_SSE2_TEST_ELF) \
		$(RING3_TIME_TEST_ELF)
	@echo "==> Creating FAT32 image..."
	@mkdir -p $(dir $@)
	@mkdir -p $(FAT32_ROOT)/docs
	@mkdir -p $(FAT32_ROOT)/boot
	@echo "This is SecureOS FAT32 storage." > $(FAT32_ROOT)/README.TXT
	@echo "IT WORKS" > $(FAT32_ROOT)/boot/pwned.txt
	@echo "FAT32 image for SecureOS." > $(FAT32_ROOT)/docs/info.txt
	cp $(RING3_TEST_ELF) $(FAT32_ROOT)/RING3.exe
	cp $(RING3_STACK_TEST_ELF) $(FAT32_ROOT)/RING3S.exe
	cp $(RING3_MMAP_TEST_ELF) $(FAT32_ROOT)/RING3M.exe
	cp $(RING3_PREEMPT_TEST_ELF) $(FAT32_ROOT)/RING3P.exe
	cp $(RING3_BIG_TEST_ELF) $(FAT32_ROOT)/RING3B.exe
	cp $(RING3_SSE_TEST_ELF) $(FAT32_ROOT)/RING3F.exe
	cp $(RING3_SSE2_TEST_ELF) $(FAT32_ROOT)/RING3G.exe
	cp $(RING3_TIME_TEST_ELF) $(FAT32_ROOT)/RING3T.exe
	truncate -s $(FAT32_IMAGE_BYTES) $@
	$(MKFS_FAT) -F 32 -n $(FAT32_VOLUME_LABEL) $@
	$(MMD) -i $@ ::/boot ::/docs
	$(MCOPY) -i $@ $(FAT32_ROOT)/README.TXT ::/README.TXT
	$(MCOPY) -i $@ $(FAT32_ROOT)/boot/pwned.txt ::/boot/PWNED.TXT
	$(MCOPY) -i $@ $(FAT32_ROOT)/docs/info.txt ::/docs/INFO.TXT
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3.exe ::RING3.EXE
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3S.exe ::RING3S.EXE
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3M.exe ::RING3M.EXE
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3P.exe ::RING3P.EXE
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3B.exe ::RING3B.EXE
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3F.exe ::RING3F.EXE
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3G.exe ::RING3G.EXE
	$(MCOPY) -i $@ $(FAT32_ROOT)/RING3T.exe ::RING3T.EXE
	@echo "==> FAT32 image created: $@"

# Declare “phony” targets.
# - A phony target is not a real file; it should always run when requested.
.PHONY: all clean run run-uefi run-uefi-serial run-debug