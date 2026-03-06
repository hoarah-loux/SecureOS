# Senior Project Os Plan


# Custom Secure Minimal OS – Senior Project Plan


## 1. Project Overview

You are building a custom minimal operating system with a strong security focus, developed primarily
on an emulator, with optional Raspberry Pi support later. The OS is not production-grade but must be
functional, well-architected, and defensible academically.

Team: - You (Computer Engineering): OS architecture, kernel, HAL, filesystem, shell - Teammate (Cyber Security): Firewall, privilege model, password system, honeypots, human-error mitigation

Primary goals:
- Bootable OS
- Minimal kernel with process + memory management
- Simple filesystem and shell (ls, cd, cat, etc.)
- Security-first architecture
- Emulator-first (QEMU), Raspberry Pi optional


## 2. High-Level Architecture

```
+-----------------------------+
|        User Programs        |
+-----------------------------+
|        User Shell           |
+-----------------------------+
|  Security Services Layer    |  <-- Firewall, honeypots, auth, privilege broker
+-----------------------------+
|    System Call Interface    |
+-----------------------------+
|         Kernel Core         |
|  (proc, mem, fs, sched)     |
+-----------------------------+
| Hardware Abstraction Layer  |
+-----------------------------+
|        Bootloader           |
+-----------------------------+
|          Hardware           |
```

Security is not an add-on — it is embedded across layers.


## 3. Development Environment

### Emulator (Primary)
QEMU (x86_64 or ARM)  
Faster debugging  
Snapshot support

### Toolchain
Cross-compiler: x86_64-elf-gcc or aarch64-none-elf-gcc  
Language: C (kernel) + minimal Assembly  
Build system: Makefile  
Debugging: gdb + QEMU

### Version Control
Git (monorepo)  
Branching: kernel, security, fs, docs


## 4. Full Directory Structure (Updated)

```
secureOS/
├── boot/
│   ├── bootloader.asm
│   ├── linker.ld
│   └── grub.cfg
│
├── kernel/
│   ├── core/
│   │   ├── kernel.c
│   │   ├── panic.c
│   │   └── syscall.c
│   │
│   ├── proc/
│   │   ├── process.c
│   │   ├── scheduler.c
│   │   └── context_switch.asm
│   │
│   ├── mm/
│   │   ├── paging.c
│   │   ├── heap.c
│   │   └── frame_alloc.c
│   │
│   ├── fs/
│   │   ├── vfs.c
│   │   ├── ramfs.c
│   │   └── fs_syscalls.c
│   │
│   ├── security/
│   │   ├── auth.c
│   │   ├── capabilities.c
│   │   ├── grant.c
│   │   └── audit.c
│   │
│   └── include/
│       └── kernel.h
│
├── hal/
│   ├── hal.c
│   ├── x86/
│   │   ├── interrupts.c
│   │   └── timer.c
│   └── arm/
│       ├── interrupts.c
│       └── timer.c
│
├── userland/
│   ├── shell/
│   │   ├── shell.c
│   │   └── commands/
│   │       ├── ls.c
│   │       ├── cd.c
│   │       └── cat.c
│   │
│   └── init/
│       └── init.c
│
├── security_services/
│   ├── privilege_broker/
│   │   ├── broker.c
│   │   ├── policy.c
│   │   ├── auth.c
│   │   ├── broker_ipc.c
│   │   └── privilege_confirm.c
│   │
│   ├── firewall/
│   │   ├── firewall.c
│   │   └── rules.c
│   │
│   ├── honeypot/
│   │   ├── fake_fs.c
│   │   └── trap_users.c
│   │
│   └── human_error/
│       ├── command_guard.c
│       └── anomaly_detector.c
│
├── scripts/
│   ├── build.sh
│   └── run_qemu.sh
│
├── docs/
│   ├── design.md
│   ├── threat_model.md
│   └── report.md
│
└── Makefile
```


## 5. Component Breakdown

### 5.1 Bootloader
Load kernel into memory  
Set CPU mode  
Multiboot compliance, memory map passing

### 5.2 Hardware Abstraction Layer (HAL)
Abstract CPU, timer, interrupts  
Allow emulator & Raspberry Pi portability  
Interrupt handling, timers, I/O abstraction

### 5.3 Kernel Core
Process Management: PCBs, scheduler, user vs kernel mode separation  
Memory Management: paging, kernel heap, user process isolation  
Filesystem: RAM FS, VFS interface, per-file permissions

### 5.4 System Call Interface
Safe boundary between userland and kernel  
Prevent direct hardware access


## 6. Security Architecture (Core Value)

### 6.1 Authentication & Password Management
Password hashing (bcrypt-like simplified)  
No plaintext passwords  
Secure credential storage

### 6.2 Privilege Model
Capability-based access  
Admin accounts designated by user attribute  
No global root; least privilege enforced  
Admin processes run unprivileged and request capabilities via the privilege broker  
Temporary capabilities granted per action; revoked automatically

### 6.3 Firewall
Kernel-level packet filtering  
Default deny  
Logging of suspicious traffic

### 6.4 Honeypot Mechanisms
Fake privileged commands  
Decoy files  
Trap processes attempting escalation

### 6.5 Human Error Mitigation Layer
Goal: Reduce damage caused by legitimate users  
Components: Dangerous command confirmation, context-aware warnings, behavior anomaly detection  
Implemented as part of the privilege broker


## 7. Privilege Elevation Flow
Admin logs in ( is_admin = true ), shell process starts unprivileged  
Admin runs command: sudo firewall --add-rule  
Shell sends request to Privilege Broker  
Broker checks authentication, policy, human-error confirmation  
Broker communicates with kernel to grant temporary capability  
Kernel enforces capability, command runs  
Capability auto-revoked; all actions logged  

No process ever has permanent root; human-error mitigation and honeypots are active.


## 8. Timeline (6 Months)
Month 1: Planning & Tooling – architecture, toolchain, bootloader  
Month 2: Kernel Skeleton – HAL, interrupts, basic memory management  
Month 3: Process + FS – scheduler, RAM filesystem, shell + ls/cd  
Month 4: Security Core – auth system, privileges, syscall hardening  
Month 5: Advanced Security – firewall, honeypots, human error mitigation  
Month 6: Polish & Docs – testing, threat model, final report, optional Raspberry Pi


## 9. Why This Will Score High
Clear separation of concerns  
Security-first OS design  
Honeypots + human error mitigation are rare in student OS projects  
Emulator-first reduces risk  
Capability-based privilege model replaces traditional root, enhancing safety


## 10. Next Steps (This Week)
Agree on CPU architecture  
Set up repository  
Bootloader → kernel print test  
Define initial admin account and password for bootstrap  
Begin kernel skeleton implementation
