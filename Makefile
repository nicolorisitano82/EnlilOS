#
# NROS Microkernel - Build System
# Target: AArch64 (ARMv8-A) - QEMU virt machine
#

# Toolchain - usa il cross-compiler AArch64
# Su macOS: brew install aarch64-elf-gcc
# Su Linux: apt install gcc-aarch64-linux-gnu
CROSS   ?= aarch64-elf-
CC       = $(CROSS)gcc
AS       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump

# Se non hai aarch64-elf-*, prova con aarch64-linux-gnu-
# CROSS ?= aarch64-linux-gnu-

# Flags
CFLAGS   = -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
           -mcpu=cortex-a72 -Iinclude
ASFLAGS  = -mcpu=cortex-a72
LDFLAGS  = -T linker.ld -nostdlib

# Sorgenti
ASM_SRCS = boot/boot.S \
           boot/vectors.S
C_SRCS   = kernel/main.c \
           kernel/microkernel.c \
           kernel/exception.c \
           kernel/mmu.c \
           kernel/pmm.c \
           kernel/kheap.c \
           kernel/gic.c \
           drivers/uart.c \
           drivers/framebuffer.c

# Oggetti
OBJS     = $(ASM_SRCS:.S=.o) $(C_SRCS:.c=.o)

# Output
KERNEL   = nros.elf
KERNEL_BIN = nros.bin

# === Targets ===

.PHONY: all clean run run-fb debug dump

all: $(KERNEL) $(KERNEL_BIN)
	@echo ""
	@echo "  ╔══════════════════════════════════════╗"
	@echo "  ║  NROS Microkernel - Build completata ║"
	@echo "  ║  Output: $(KERNEL)                ║"
	@echo "  ╚══════════════════════════════════════╝"
	@echo ""

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

$(KERNEL_BIN): $(KERNEL)
	$(OBJCOPY) -O binary $< $@

%.o: %.S
	$(AS) $(ASFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Esegui con QEMU (solo output seriale UART)
run: $(KERNEL)
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a72 \
		-m 512M \
		-nographic \
		-kernel $(KERNEL)

# Esegui con QEMU + framebuffer (output grafico)
run-fb: $(KERNEL)
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a72 \
		-m 512M \
		-device ramfb \
		-serial stdio \
		-kernel $(KERNEL)

# Debug con GDB
debug: $(KERNEL)
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a72 \
		-m 512M \
		-nographic \
		-kernel $(KERNEL) \
		-S -s &
	@echo "QEMU avviato in attesa di GDB sulla porta 1234"
	@echo "Connetti con: aarch64-elf-gdb -ex 'target remote :1234' $(KERNEL)"

# Disassembly
dump: $(KERNEL)
	$(OBJDUMP) -d $< | head -100

clean:
	rm -f $(OBJS) $(KERNEL) $(KERNEL_BIN)
	@echo "Clean completato."
