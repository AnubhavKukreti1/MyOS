# MyOS - Makefile
#
# make              build build/kernel.elf
# make iso          build the bootable myos.iso
# make run          boot it in QEMU (BIOS), serial log in this terminal
# make run-serial   same, but no window: everything on this terminal
# make run-uefi     boot it through UEFI (needs the edk2 firmware from Homebrew QEMU)
# make debug        start QEMU paused, waiting for gdb on :1234
# make clean        remove build output

# ---- Toolchain (macOS: brew install x86_64-elf-gcc qemu xorriso) ----
ARCH_PREFIX ?= x86_64-elf-
CC          := $(ARCH_PREFIX)gcc
LD          := $(ARCH_PREFIX)ld
QEMU        ?= qemu-system-x86_64
XORRISO     ?= xorriso

# ---- Limine bootloader binaries (cloned automatically by `make limine`) ----
LIMINE_DIR    ?= limine-bin
LIMINE_BRANCH ?= v9.x-binary

KERNEL := build/kernel.elf
ISO    := myos.iso

# ---- Flags ----
# -mcmodel=kernel + linker.ld : kernel lives in the top 2 GiB
# -mno-red-zone               : interrupts would corrupt the red zone
# -mgeneral-regs-only         : no SSE/x87 in kernel code (not enabled yet)
# -fno-omit-frame-pointer     : panic backtraces walk the RBP chain
# -fno-tree-loop-distribute-patterns : GCC must not turn loops into memcpy calls
CFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-PIC -fno-pie -fno-lto -fno-omit-frame-pointer \
          -fno-tree-loop-distribute-patterns \
          -m64 -march=x86-64 -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -Wall -Wextra -Werror
CPPFLAGS := -Iinclude -MMD -MP
LDFLAGS  := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T linker.ld

# Every .c / .S under src/ is built automatically; empty placeholder files are fine.
SRC_C := $(shell find src -name "*.c")
SRC_S := $(shell find src -name "*.S")
OBJ   := $(SRC_C:%.c=build/%.o) $(SRC_S:%.S=build/%.o)
DEP   := $(OBJ:.o=.d)

.PHONY: all iso run run-serial run-uefi debug limine clean distclean

all: $(KERNEL)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJ) -o $@

# ---- Limine ----
limine: $(LIMINE_DIR)/limine

$(LIMINE_DIR)/limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=$(LIMINE_BRANCH) --depth=1 $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR)

# ---- Bootable ISO (BIOS + UEFI hybrid) ----
iso: $(ISO)

$(ISO): $(KERNEL) limine.conf $(LIMINE_DIR)/limine
	rm -rf iso_root
	mkdir -p iso_root/boot/limine iso_root/EFI/BOOT
	cp $(KERNEL) iso_root/boot/kernel.elf
	cp limine.conf iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	   $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI iso_root/EFI/BOOT/
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
	   -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	   -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
	   -efi-boot-part --efi-boot-image --protective-msdos-label \
	   iso_root -o $(ISO)
	./$(LIMINE_DIR)/limine bios-install $(ISO)

# ---- Running ----

DISK := disk.img
MKFS := $(shell command -v mkfs.fat 2>/dev/null || command -v mkfs.vfat)

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=32
	$(MKFS) -F 32 $(DISK)

# -no-reboot / -no-shutdown: a triple fault freezes QEMU instead of rebooting
#   in a loop, so you can read the last output.
QEMU_COMMON := -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown -boot d

run: $(ISO) $(DISK)
	$(QEMU) $(QEMU_COMMON) \
		-drive id=hd0,file=$(DISK),format=raw,if=none \
		-device ahci,id=ahci \
		-device ide-hd,drive=hd0,bus=ahci.0

run-serial: $(ISO) $(DISK)
	$(QEMU) $(QEMU_COMMON) -display none \
		-drive id=hd0,file=$(DISK),format=raw,if=none \
		-device ahci,id=ahci \
		-device ide-hd,drive=hd0,bus=ahci.0

OVMF ?= $(shell brew --prefix qemu 2>/dev/null)/share/qemu/edk2-x86_64-code.fd
run-uefi: $(ISO)
	$(QEMU) $(QEMU_COMMON) -drive if=pflash,format=raw,readonly=on,file=$(OVMF)

debug: $(ISO)
	$(QEMU) $(QEMU_COMMON) -s -S -d int,cpu_reset -D qemu.log

clean:
	rm -rf build iso_root $(ISO) qemu.log disk.img
distclean: clean
	rm -rf $(LIMINE_DIR)

-include $(DEP)