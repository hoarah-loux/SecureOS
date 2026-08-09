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

# GRUB ISO builder.
# - grub-mkrescue creates a bootable ISO image containing GRUB and our files.
GRUB_MKRESCUE = grub-mkrescue

# ---------------- GRUB platform selection (UEFI ISO) ----------------

# Directory containing GRUB UEFI modules for x86_64.
# - `?=` means “set this only if it is not already set by the environment”.
# - This lets you override it like:
#     make GRUB_PLATFORM_DIR=/some/other/path
GRUB_PLATFORM_DIR ?= /usr/lib/grub/x86_64-efi

# Flags passed to grub-mkrescue.
# - `-d <dir>` tells grub-mkrescue where to find the platform modules.
GRUB_MKRESCUE_FLAGS ?= -d $(GRUB_PLATFORM_DIR)

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
# - `-mcmodel=kernel`:
#     Use the kernel code model (assumptions suitable for kernel addresses).
# - `-mno-mmx -mno-sse -mno-sse2`:
#     Avoid using MMX/SSE registers/instructions (no FPU/SIMD init yet).
# - `-fno-pic -fno-pie`:
#     Do not emit position-independent code/executable; we link at fixed VAs.
# - `-Wall -Wextra`:
#     Enable common and extra warnings.
CFLAGS = -ffreestanding -O0 -g3 -nostdlib -m64 -mno-red-zone -mcmodel=kernel \
		 -mno-mmx -mno-sse -mno-sse2 -fno-pic -fno-pie -Wall -Wextra \
		 -I$(INCLUDE_DIR)

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
	$(SRC_DIR)/arch/x86_64/interrupts.asm

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
	$(KERNEL_DIR)/proc/loader/loader.c

ARCH_C_SRCS = \
	$(SRC_DIR)/arch/x86_64/idt.c

# Linker script.
LINKER_SCRIPT = $(BOOT_DIR)/grub/linker.ld

# GRUB configuration file.
GRUB_CFG = $(BOOT_DIR)/grub/grub.cfg

# ---------------- Object files (intermediate build artifacts) ----------------

# Object file produced by assembling the entry stub.
ASM_OBJS = \
	$(BUILD_DIR)/entry64_mb2.o \
	$(BUILD_DIR)/arch/x86_64/interrupts.o

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

# Create the bootable ISO image.
# - grub-mkrescue collects GRUB modules and our staged tree and emits an ISO.
$(ISO_FILE): $(KERNEL_ELF) $(GRUB_CFG)
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


# ---------------- FAT32 image build ----------------

# Rule to build the FAT32 image.
$(FAT32_IMAGE):
	@echo "==> Creating FAT32 image..."
	@mkdir -p $(dir $@)
	@mkdir -p $(FAT32_ROOT)/docs
	@mkdir -p $(FAT32_ROOT)/boot
	@echo "This is SecureOS FAT32 storage." > $(FAT32_ROOT)/README.TXT
	@echo "IT WORKS" > $(FAT32_ROOT)/boot/pwned.txt
	@echo "FAT32 image for SecureOS." > $(FAT32_ROOT)/docs/info.txt
	cp $(PROJECT_ROOT)/elf.elf $(FAT32_ROOT)/ELF.exe
	truncate -s $(FAT32_IMAGE_BYTES) $@
	$(MKFS_FAT) -F 32 -n $(FAT32_VOLUME_LABEL) $@
	$(MMD) -i $@ ::/boot ::/docs
	$(MCOPY) -i $@ $(FAT32_ROOT)/README.TXT ::/README.TXT
	$(MCOPY) -i $@ $(FAT32_ROOT)/boot/pwned.txt ::/boot/PWNED.TXT
	$(MCOPY) -i $@ $(FAT32_ROOT)/docs/info.txt ::/docs/INFO.TXT
	$(MCOPY) -i $@ $(FAT32_ROOT)/ELF.exe ::ELF.EXE
	@echo "==> FAT32 image created: $@"

# Declare “phony” targets.
# - A phony target is not a real file; it should always run when requested.
.PHONY: all clean run run-uefi run-uefi-serial run-debug
