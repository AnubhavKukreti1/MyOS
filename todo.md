OS Development Roadmap — TODO
Stage 1 — Bootloader

Status: 🚧 In Progress

Build Limine
Understand BIOS booting
Understand UEFI booting
Stage 2 — Your First Kernel

Status: ⏳ Next

Write a freestanding C kernel
Boot the kernel through Limine
Print something on the screen
Stage 3 — CPU and Interrupts

Status: ⬜ Not Started

GDT (Global Descriptor Table)
IDT (Interrupt Descriptor Table)
Exceptions
Hardware interrupts
Timer
Stage 4 — Memory Management

Status: ⬜ Not Started

Physical Memory Manager (PMM)
Virtual Memory
Paging
Kernel Heap
Stage 5 — Multitasking

Status: ⬜ Not Started

Threads
Context Switching
Scheduler
Synchronization
Stage 6 — Hardware and Storage

Status: ⬜ Not Started

Keyboard
Display
Disk Access
Filesystem
Stage 7 — Userspace

Status: ⬜ Not Started

System Calls
User Mode
Programs
Shell
Stage 8 — Advanced OS

Status: ⬜ Not Started

Networking
USB Support
GUI (Graphical User Interface)
Applications
Overall Progress
Stage 1 — Bootloader
Stage 2 — First Kernel
Stage 3 — CPU and Interrupts
Stage 4 — Memory Management
Stage 5 — Multitasking
Stage 6 — Hardware and Storage
Stage 7 — Userspace
Stage 8 — Advanced OS

Goal: Build a complete operating system from scratch.

Milestone 1: Proper kernel initialization

Current stage
Your kernel should initialize itself and obtain information from Limine.

Milestone 2: Limine memory map

Obtain and print all available physical memory regions.

Milestone 3: Physical Memory Manager (PMM)

Implement a bitmap-based page allocator using 4 KiB pages.

Milestone 4: Virtual Memory Manager (VMM)

Understand and manage x86_64 page tables.

Milestone 5: Kernel Heap

Implement kmalloc() and kfree().