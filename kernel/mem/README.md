# Physical Memory Manager (PMM)

A robust bitmap-based physical frame allocator designed for bare-metal x86/x86_64 kernels using Limine bootloader protocol data.

- **Granularity:** 4KB frames (`PAGE_SIZE`)
- **Strategy:** First-fit with search pointer optimization
- **Safety:** Prevents double frees, guards lower 1MiB, tracks kernel and reclaimable structures.
