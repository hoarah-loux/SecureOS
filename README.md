# SecureOS

**A security-oriented, clean-slate operating system for x86-64 · v1**

SecureOS is an operating-system kernel developed from scratch as a research operating system. It explores how a modular operating system can incorporate memory protection, privilege separation, and fault containment into its core architecture instead of treating security as a collection of optional add-ons.

SecureOS has been developed and validated in **QEMU** and has also been tested on **real hardware**, where it boots and operates normally. Version **1 (v1)** brings together the core kernel architecture and the user-space execution environment: memory management, scheduling, system calls, persistent storage, keyboard input, and framebuffer output work together to support statically linked applications running in ring 3.

> **Project status:** SecureOS v1 is a functional research operating system. It is not positioned as a production-ready security platform; its implemented capabilities, current scope, and planned extensions are described separately below.

---

## Table of Contents

- [Goals](#goals)
- [Highlights](#highlights)
- [Architecture](#architecture)
- [Boot sequence](#boot-sequence)
- [Memory management](#memory-management)
- [Processes, threads, and scheduling](#processes-threads-and-scheduling)
- [User space and system calls](#user-space-and-system-calls)
- [Storage and file system](#storage-and-file-system)
- [Console, keyboard, and graphics](#console-keyboard-and-graphics)
- [Networking status](#networking-status)
- [Security model](#security-model)
- [Repository layout](#repository-layout)
- [Building](#building)
- [Running in QEMU](#running-in-qemu)
- [Using the system](#using-the-system)
- [Testing and validation](#testing-and-validation)
- [Current scope and implementation boundaries](#current-scope-and-implementation-boundaries)
- [Future work](#future-work)
- [Contributing](#contributing)
- [Project background](#project-background)

## Goals

SecureOS was created to investigate whether a clean-slate kernel can provide a coherent and extensible foundation for operating-system research while keeping its implementation understandable and modular.

The main goals are to:

1. Build a bootable 64-bit operating system with a controlled firmware-to-kernel handoff.
2. Implement physical and virtual memory management, including demand paging.
3. Provide isolated process address spaces and a preemptive thread scheduler.
4. Expose kernel services through a defined user-to-kernel system-call interface.
5. Support persistent files and ordinary compiled C applications.
6. Enforce foundational memory and privilege protections in the kernel architecture.
7. Keep subsystem boundaries clear so that future capabilities can be added incrementally.

## Highlights

- **x86-64 kernel:** boots through a UEFI/GRUB/Multiboot2 path and enters 64-bit long mode.
- **Higher-half layout:** the kernel uses a higher-half virtual address layout and a direct physical-memory map.
- **Layered memory management:** bootstrap memory discovery, bitmap tracking, a buddy frame allocator, four-level paging, virtual regions, demand paging, and a slab-backed kernel heap.
- **Protected execution:** independent process address spaces, ring-3 application execution, separate user and kernel stacks, and controlled privilege transitions.
- **Preemptive scheduling:** timer-driven scheduling, context switching, sleep queues, and blocking keyboard reads.
- **ELF64 loading:** validates executable metadata and creates file-backed mappings for statically linked applications.
- **System calls:** a bounded dispatcher entered through software interrupt vector `0x80`.
- **FAT32 storage:** IDE PIO-backed block access and basic file and directory operations.
- **User-space C runtime:** newlib integration and a collection of C applications, including a Doom port used as a broad integration workload.
- **Text and graphics I/O:** serial diagnostics, a framebuffer console, interrupt-driven keyboard input, and a user-space framebuffer presentation interface.

## Architecture

SecureOS is a monolithic kernel: core services such as memory management, scheduling, interrupt handling, executable loading, and filesystem operations execute in kernel space. User applications execute with reduced privilege and request kernel services through the system-call boundary.

```text
+------------------------------------------------------+
|                  User applications                   |
|       C utilities, tests, interactive programs       |
+------------------------------------------------------+
|              Newlib / user-space runtime             |
+------------------------------------------------------+
|             System-call ABI (int 0x80)               |
+------------------------------------------------------+
|                    Kernel (ring 0)                   |
|                                                      |
|  Process & thread manager       Scheduler            |
|  ELF loader                     Interrupt handlers   |
|  Virtual memory                 FAT32 / block I/O    |
|  Physical memory & heap         Console / input      |
+------------------------------------------------------+
|                 x86-64 architecture                 |
|       Paging | GDT | IDT | TSS | PIT | PIC           |
+------------------------------------------------------+
|              GRUB / Multiboot2 / UEFI                |
+------------------------------------------------------+
|                     Hardware                         |
+------------------------------------------------------+
```

The key integration path is:

1. The bootloader loads the kernel and supplies Multiboot2 information.
2. The kernel establishes its permanent paging layout and initializes physical memory management.
3. The virtual-memory manager creates process address spaces and records virtual regions.
4. The ELF loader validates an executable and creates file-backed regions and a user stack.
5. The scheduler dispatches the new thread and enters ring 3.
6. Page faults materialize demand-paged memory as the application accesses it.
7. Applications use system calls for file I/O, timing, keyboard input, memory growth, process exit, and graphics presentation.

## Boot sequence

The boot pipeline uses UEFI firmware, GRUB, and the Multiboot2 protocol. GRUB loads the kernel ELF image and prepares boot information, including the memory map and framebuffer details. A dedicated assembly entry path then establishes the processor state expected by the kernel.

At a high level:

1. UEFI firmware starts GRUB.
2. GRUB loads the SecureOS kernel and prepares Multiboot2 information.
3. The kernel entry stub disables interrupts, establishes an initial stack and GDT, and builds bootstrap paging structures.
4. The processor transitions into x86-64 long mode.
5. Early initialization clears BSS, validates boot information, and hands control to the C kernel entry point.
6. The kernel initializes memory management, interrupt handling, storage, process management, and the shell.

The initial page tables provide the mappings needed for the transition; the virtual-memory subsystem later constructs the permanent higher-half layout. Serial output is used for early diagnostics, with framebuffer output available after the display information has been initialized.

## Memory management

Memory management is divided into cooperating layers so that each stage can operate with only the facilities available at that point in boot.

### Physical memory

- **Bootstrap discovery and bitmap:** the kernel parses the Multiboot2 memory map, tracks usable frames, and reserves memory occupied by the kernel, boot information, and allocator metadata.
- **Buddy allocator:** runtime physical allocations use power-of-two blocks. Larger blocks can be split to satisfy requests, and compatible free buddies can be coalesced on release.
- **Direct physical map:** after the permanent address space is active, physical memory and paging structures can be accessed through a stable higher-half mapping.

### Virtual memory

The virtual-memory manager uses x86-64 four-level paging (PML4, PDPT, PD, and PT). It supports address-space creation, page-table traversal, mapping and unmapping, translation, and virtual-region metadata.

Each process receives its own page-table root. User mappings are process-specific, while the kernel's higher-half mappings are present in each address space with supervisor-only permissions. This lets the CPU enter the kernel from different processes without exposing kernel pages to ring-3 code.

### Demand paging

Virtual regions can be reserved before physical frames are allocated. When a process first accesses a valid but unmapped page, the page-fault handler:

1. Identifies the faulting address and access type.
2. Locates the owning process and matching virtual region.
3. Obtains a physical frame.
4. Initializes the page from its backing source (anonymous zero-filled memory or executable file data).
5. Installs the page-table mapping with the region's permissions.
6. Returns to the faulting instruction.

The implementation includes anonymous and file-backed demand paging, ELF segment mappings, demand-paged user stacks, and a reserved process heap arena.

### Kernel heap

A kernel heap is built over reserved virtual memory. Slab caches provide fixed-size allocation classes for frequently used kernel objects, while the physical allocator supplies the backing frames.

## Processes, threads, and scheduling

SecureOS separates resource ownership from schedulable execution:

- A **process** owns an address space, virtual-region metadata, executable backing, file descriptors, heap boundaries, and its collection of threads.
- A **thread** owns the saved processor context, instruction and stack pointers, kernel and user stacks, scheduling state, and floating-point/SSE state.

The scheduler is preemptive and driven by timer interrupts. It maintains runnable and waiting states, supports sleeping until a deadline, and wakes blocked keyboard readers when input arrives. Blocking operations do not need to spin continuously while waiting for an event.

Context switching preserves the processor state needed to resume a thread, switches to the owning process's address space, and updates the Task State Segment's kernel-stack pointer for privilege transitions. The scheduler and surrounding architecture provide a foundation for multiprocessor support. SecureOS v1 currently operates with one active processor; bringing up and coordinating multiple processors is the next immediate development milestone.

User-mode faults, such as invalid memory accesses and protection violations, are attributed to the offending process and routed through process termination. Kernel-mode faults remain fatal because continuing after corruption of privileged state cannot be assumed safe.

## User space and system calls

SecureOS loads statically linked ELF64 applications and starts them in ring 3. The loader validates the ELF header, program-header table, segment bounds, entry point, and relevant alignment constraints before creating mappings. Segment permissions are derived from ELF flags. The executable remains available as backing storage while file-backed pages may still need to be loaded.

### System-call ABI

Applications enter the kernel through software interrupt vector `0x80`. The system-call number is passed in `RAX`; arguments are passed in `RDI`, `RSI`, `RDX`, `R10`, `R8`, and `R9`; and the result is returned in `RAX`.

The dispatcher checks the requested number against a bounded dispatch table. User pointers are treated as untrusted and validated before kernel code reads or writes application memory.

The implemented interface includes services for:

- File open, read, write, close, and seek operations.
- Keyboard input, including raw input support.
- Process termination.
- Anonymous and file-backed memory mapping.
- Program-break/heap growth.
- Time retrieval and sleeping.
- Framebuffer presentation.

Some conventional process-control interfaces remain stubs or are not exposed through the ordinary syscall table. In particular, SecureOS does not currently provide the full Unix `fork`/`exec`/`wait` process model.

### C runtime

The user-space startup code establishes the required ABI stack alignment, initializes runtime state, invokes constructors, calls `main()`, and routes termination through the kernel. A newlib adaptation layer connects standard C library operations to SecureOS services, including formatted output, file operations, heap allocation, time functions, and process exit.

The runtime is intentionally small. Executables are statically linked; dynamic linking is not supported, and argument/environment setup is limited.

## Storage and file system

Persistent storage is accessed through a block-device abstraction backed by an IDE PIO driver. The active filesystem path uses FAT32.

The FAT32 implementation validates volume metadata and supports:

- Mounting and volume-geometry validation.
- Root and nested-directory traversal.
- Path resolution and directory listing.
- File creation and opening.
- Reading, seeking, writing, and appending.
- Truncation and deletion.

Each process has its own descriptor table. Standard input, output, and error occupy reserved entries, and the loader retains a protected descriptor for the executable while demand-paged file-backed regions may still need it.

The repository also contains TAR-related support, but FAT32 on the IDE-backed path is the active persistent filesystem described by the project report. The current storage driver uses synchronous polling-based PIO rather than asynchronous DMA-based I/O.

## Console, keyboard, and graphics

### Console and diagnostics

SecureOS provides serial output for early boot and kernel diagnostics, alongside a framebuffer-based console. The console supports character rendering, cursor movement, line wrapping, backspace, scrolling, and configurable colors.

### Keyboard

Keyboard input is interrupt-driven. The kernel collects scan codes, buffers input, and supports cooked character input as well as raw key events for applications such as games. Foreground ownership prevents unrelated processes from consuming another application's keyboard input. Blocking reads put the caller to sleep until input arrives.

### Framebuffer presentation

Applications draw into a user-space pixel buffer and request presentation through a system call. The kernel validates the dimensions and buffer range, then copies and scales the image into the privileged framebuffer. Applications do not receive direct access to the physical framebuffer mapping.

## Networking status

The source tree contains a layered networking implementation, including an RTL8139 driver and Ethernet, ARP, IPv4, ICMP, and UDP components. The code includes packet validation, address resolution, checksums, and callback-based UDP receive handling.

**Important:** the project report's source review states that this networking code is not connected to the active kernel build and initialization path, and that no user-space socket/datagram syscall interface is present. Therefore, the current booted configuration should not be described as providing working end-to-end networking. TCP, IP fragmentation/reassembly, and network security services are also absent.

## Security model

Security in the current implementation is primarily enforced through processor privilege levels and memory-management rules.

| Mechanism | Purpose |
|---|---|
| Ring-3 user execution | Runs applications at a lower privilege level than the kernel. |
| Independent process address spaces | Separates user mappings between processes. |
| Supervisor-only kernel mappings | Prevents ring-3 code from directly accessing kernel pages. |
| NX protection | Prevents execution from pages not marked executable. |
| W^X-oriented page permissions | Keeps writable data separate from executable code where mappings are established. |
| CR0.WP write protection | Enforces read-only page permissions during supervisor execution. |
| User-pointer validation | Checks application-provided buffers and strings before kernel access. |
| ELF validation | Rejects malformed executable metadata and out-of-range segments. |
| Zero-filled anonymous pages | Prevents newly allocated anonymous memory from exposing stale frame contents. |
| User-fault containment | Terminates a faulty user process instead of automatically halting the entire kernel. |
| Separate user/kernel stacks | Keeps privileged exception and interrupt handling on a kernel-controlled stack. |

These are meaningful low-level protections, but they do not make SecureOS a production security platform. The project does not currently implement a complete authentication and identity system, filesystem ownership/ACLs, cryptographic boot or executable verification, a mature audit system, full ASLR, syscall filtering, comprehensive sandbox policies, or active network filtering.

AI-assisted anomaly detection, honeypots, firewall policy services, and broader security-management features are future goals rather than completed operational subsystems.

## Repository layout

The repository is organized around the kernel source, architecture-specific code, shared headers, boot assets, user applications, and supporting material. The build rules identify the following key paths:

```text
SecureOS/
├── assets/
│   └── elf_tests/          # Hand-written ring-3 ELF test programs
├── backups/                # Development backups
├── docs/                   # Project documentation
├── hdr/                    # Shared kernel and ABI headers
├── src/
│   ├── boot/
│   │   └── grub/           # Multiboot2 entry, GRUB config, linker script
│   ├── arch/
│   │   └── x86_64/         # Interrupt/context-switch assembly and CPU setup
│   ├── kernel/
│   │   ├── core/           # Kernel entry, console, serial, shell, framebuffer
│   │   ├── interrupts/     # Exception, keyboard, PIC, syscall handling
│   │   ├── mm/
│   │   │   └── alloc/      # PMM, buddy, slab, heap, VMM
│   │   ├── fs/             # Block device, IDE PIO, FAT32, TAR, filesystem calls
│   │   └── proc/
│   │       ├── ds/         # Process and thread data structures
│   │       ├── loader/     # ELF loader
│   │       └── sched/      # Scheduler, queues, context management
│   └── userland/
│       ├── apps/           # User-space applications
│       └── libc_glue/      # Newlib syscall/runtime adaptation
├── Makefile                # Kernel, userland, disk-image and ISO build rules
└── README.md
```

The exact contents may evolve. The Makefile is the authoritative reference for the source files included in the active build.

## Building

### Host requirements

The build is driven by GNU Make and uses a freestanding GCC toolchain, NASM, GNU binutils, and GRUB's ISO-generation utilities. Running the system requires QEMU. The UEFI run targets also require OVMF firmware files.

On a Debian/Ubuntu-based host, the following package set covers the tools referenced by the build rules on a typical installation:

```bash
sudo apt update
sudo apt install build-essential gcc binutils make nasm grub-common grub-pc-bin \
  grub-efi-amd64-bin xorriso mtools dosfstools qemu-system-x86 ovmf curl
```

Package names and OVMF firmware paths can vary by distribution. The Makefile defaults to:

- `/usr/lib/grub/x86_64-efi` for GRUB's x86-64 EFI modules.
- `/usr/share/OVMF/OVMF_CODE_4M.fd` for UEFI firmware code.
- `/usr/share/OVMF/OVMF_VARS_4M.fd` for the writable UEFI variables template.

If your distribution installs these files elsewhere, override the corresponding Make variables when invoking `make`.

### Clone and build

```bash
git clone https://github.com/hoarah-loux/SecureOS.git
cd SecureOS
make
```

The default target (`make`) builds the kernel and bootable ISO at the repository root:

```text
secureos-uefi.iso
```

The intermediate objects and staged ISO tree are placed under `src/boot/build/`. The QEMU run targets additionally build userland applications and a FAT32 disk image.

### Clean build artifacts

```bash
make clean
```

This removes the generated build directory and ISO. It does not remove source files.

## Running in QEMU

### UEFI boot (recommended path)

```bash
make run-uefi
```

This target builds the userland applications and FAT32 disk image if needed, then launches `qemu-system-x86_64` with OVMF firmware, the generated ISO, the FAT32 disk image, and 2 GiB of guest memory.

The FAT32 image includes the Doom data file. If `assets/DOOM1.WAD` is absent, the Makefile downloads the expected shareware v1.9 file and verifies its SHA-256 checksum before using it. The WAD is not committed to the repository.

### UEFI boot with serial output

```bash
make run-uefi-serial
```

This redirects the guest serial port to the host terminal and is useful for observing boot diagnostics and kernel test output.

### Default QEMU run

```bash
make run
```

This starts QEMU with the generated ISO and FAT32 disk image using the default firmware configuration.

### Debug with GDB

```bash
make run-debug
```

The debug target starts QEMU paused with a GDB stub enabled (`-s -S`). In another terminal, connect with:

```bash
gdb
(gdb) target remote :1234
```

Use the kernel ELF and available debug symbols when setting breakpoints. The exact GDB executable/target configuration may need adjustment for your local toolchain.

## Using the system

After boot, SecureOS presents its kernel shell. The shell provides filesystem and process-launching functionality. The report describes support for filesystem inspection and modification commands and launching executable paths prefixed with `./`.

The userland application set includes utilities and test programs for standard output, file operations, dynamic allocation, timing, keyboard input, framebuffer presentation, and fault handling. A Doom port is included as a demanding integration workload that exercises several of these interfaces together.

The exact command list and available application filenames are determined by the applications copied into the FAT32 image during the current build. Use the shell's built-in help, if available in the checked-out revision, to inspect commands rather than assuming a fixed command set.

## Testing and validation

The project report documents targeted tests across subsystem and integration boundaries, including:

- Physical frame allocation, release, and reuse.
- Virtual-address translation and higher-half mappings.
- FAT32 mount validation, path resolution, file reads, and write/append/seek/truncate/delete cycles.
- System-call dispatch validation and invalid-call handling.
- ELF loading and ring-3 entry.
- Timer-driven preemption and interleaved process progress.
- Demand-paged user stacks and file-backed executable mappings, including BSS zero-fill.
- SSE/x87 state preservation across context switches.
- Monotonic time and sleep/wake-up behavior.
- Invalid-pointer handling and user-process fault containment.
- Keyboard delivery and framebuffer presentation.
- Larger user-space workloads, including Doom.

Much of the current validation is observed through serial diagnostics and application output. The report notes that a centralized automated regression harness with reproducible pass/fail records across emulator configurations remains future work.

## Current scope and implementation boundaries

SecureOS v1 focuses on a coherent, working kernel and user-space environment. The items below describe capabilities that are outside the current v1 scope or are present in the source tree but are not yet part of the active runtime. They are not a list of general project shortcomings; several are natural next steps for an operating system that is being developed incrementally:

- **Single-processor execution:** v1 uses one active CPU. SMP bring-up, inter-processor coordination, and multiprocessor scheduling are the immediate next milestone.
- **Limited process-control API:** there is no complete Unix-style `fork`, `exec`, and `wait` model; some conventional calls remain stubs.
- **Static executables:** dynamic linking, shared libraries, and full position-independent executable support are not available in the described runtime.
- **Synchronous storage:** IDE PIO uses polling and can occupy the processor during transfers; asynchronous DMA and interrupt-driven completion are future improvements.
- **FAT32 constraints:** no journaling, filesystem ACLs, encryption, or robust crash-recovery mechanism is provided.
- **Networking not active end-to-end:** protocol and driver code exists but is not integrated into the active kernel path.
- **Incomplete higher-level security services:** authentication, identities, capabilities, auditing, sandboxing, full ASLR, and active network filtering are not implemented as a complete system.
- **Test automation:** subsystem tests exist, but a unified automated regression and metrics framework remains incomplete.

SecureOS remains a research and educational operating system rather than a hardened general-purpose platform. The current security mechanisms should be understood in the context of the features listed here; they are not a guarantee of protection against malicious code or real-world attacks.

## Future work

The next stage of SecureOS development will extend the v1 foundation while preserving its existing kernel, memory, scheduler, and filesystem abstractions.

### Immediate next step: SMP support

The first priority after v1 is **symmetric multiprocessing (SMP)**. The goal is to bring up additional processor cores and evolve the current single-CPU execution model into a coordinated multiprocessor kernel. This work is expected to include:

- Discovering and starting application processors (APs).
- Initializing per-CPU state and local interrupt-controller support.
- Introducing the synchronization primitives needed to protect shared kernel structures.
- Adding inter-processor interrupts (IPIs) and cross-CPU coordination.
- Extending scheduling with per-CPU runnable queues, CPU affinity, and load balancing as appropriate.
- Coordinating address-space changes and TLB invalidation across processors.

SMP is the immediate next milestone because it expands the execution foundation on which later performance and concurrency work can build.

### Further development

Following the SMP milestone, planned work includes:

1. **Reliability and regression testing:** build a centralized automated test harness, record repeatable pass/fail results, and add longer-running stress tests and useful performance measurements.
2. **Kernel and memory-safety hardening:** strengthen resource-lifetime handling, mapping validation, diagnostics, and fault reporting.
3. **Storage improvements:** introduce DMA and asynchronous or interrupt-driven I/O to reduce processor time spent in synchronous IDE PIO operations.
4. **Process and runtime facilities:** expand process lifecycle management and add richer process-control interfaces, alongside further executable-format and runtime support.
5. **Networking integration:** connect the existing RTL8139 and protocol implementation to the active kernel path, then extend the stack with TCP and IP fragmentation/reassembly.
6. **Security services:** develop concrete policy and monitoring facilities, potentially including system-call behavior monitoring, honeypot mechanisms, and lightweight AI-assisted anomaly detection.
7. **Hardware compatibility:** continue validating drivers and boot behavior across a wider range of physical machines and firmware configurations.


The project report credits the following team contributions:

- **Hoarah Loux:** bootloader, physical/virtual memory management, heap, process management, user space, and interrupt handling.
- **5pky:** filesystem, physical memory management, user space, interrupt handling, shell, and system-call interface.
- **FalconABD:** networking stack and user space.
- **TheWmountain:** networking stack and user space.

---

*SecureOS is a research project. Its security mechanisms are experimental and should not be interpreted as a guarantee of protection against malicious code or real-world attacks.*
