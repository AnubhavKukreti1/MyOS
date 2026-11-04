
LLVM_PREFIX := $(shell brew --prefix llvm)
LLD_PREFIX := $(shell brew --prefix lld)

CLANG := $(LLVM_PREFIX)/bin/clang
LD := $(LLD_PREFIX)/bin/ld.lld

BUILD := build

CFLAGS := \
    --target=x86_64-unknown-none-elf \
    -std=gnu11 \
    -O2 \
    -Wall \
    -Wextra \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic \
    -fno-pie \
    -fno-builtin \
    -mno-red-zone \
    -mcmodel=kernel \
    -fno-asynchronous-unwind-tables \
    -fno-unwind-tables

all: $(BUILD)/myos.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/main.o: kernel/main.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c kernel/main.c -o $(BUILD)/main.o

$(BUILD)/myos.elf: $(BUILD)/main.o linker.ld
	$(LD) -m elf_x86_64 \
	    -z max-page-size=0x1000 \
	    -T linker.ld \
	    -o $(BUILD)/myos.elf \
	    $(BUILD)/main.o

clean:
	rm -rf $(BUILD)

.PHONY: all clean
