#
# EnlilOS Microkernel - Build System
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
           -mcpu=cortex-a72 -Iinclude -Idrivers/ane -Idrivers/gpu
ASFLAGS  = -mcpu=cortex-a72
LDFLAGS  = -T linker.ld -nostdlib

# Sorgenti
ASM_SRCS = boot/boot.S \
           boot/vectors.S \
           kernel/sched_switch.S
C_SRCS   = kernel/main.c \
           kernel/microkernel.c \
           kernel/exception.c \
           kernel/mmu.c \
           kernel/pmm.c \
           kernel/kheap.c \
           kernel/gic.c \
           kernel/timer.c \
           kernel/sched.c \
           kernel/tty.c \
           kernel/string.c \
           kernel/ext4.c \
           kernel/vfs.c \
           kernel/syscall.c \
           kernel/ane_syscall.c \
           kernel/gpu_syscall.c \
           kernel/utf8.c \
           drivers/ane/ane_hw.c \
           drivers/ane/ane_sw.c \
           drivers/gpu/gpu_agx.c \
           drivers/gpu/gpu_sw.c \
           drivers/gpu/gpu_virtio.c \
           drivers/uart.c \
           drivers/keyboard.c \
           drivers/mouse.c \
           drivers/blk.c \
           drivers/framebuffer.c

# Oggetti
OBJS     = $(ASM_SRCS:.S=.o) $(C_SRCS:.c=.o)

# Output
KERNEL   = enlil.elf
KERNEL_BIN = enlil.bin

# === Targets ===

.PHONY: all clean run run-fb run-gpu run-blk debug dump disk-ready disk-reset disk-fsck

all: $(KERNEL) $(KERNEL_BIN)
	@echo ""
	@echo "  ╔══════════════════════════════════════╗"
	@echo "  ║  EnlilOS Microkernel - Build completata ║"
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
		-accel tcg \
		-cpu cortex-a72 \
		-m 512M \
		-nographic \
		-kernel $(KERNEL)

# Esegui con QEMU + framebuffer RAMFB (SW backend, output grafico)
run-fb: $(KERNEL)
	qemu-system-aarch64 \
		-machine virt \
		-accel tcg \
		-cpu cortex-a72 \
		-m 512M \
		-vga none \
		-global virtio-mmio.force-legacy=false \
		-device ramfb \
		-device virtio-keyboard-device \
		-display default,show-cursor=on \
		-monitor none \
		-serial stdio \
		-kernel $(KERNEL)

# Esegui con QEMU + VirtIO-GPU (M5b-01, backend virtio)
# Apre finestra grafica via SDL/Cocoa; -serial stdio per UART su terminale.
run-gpu: $(KERNEL)
	qemu-system-aarch64 \
		-machine virt \
		-accel tcg \
		-cpu cortex-a72 \
		-m 512M \
		-vga none \
		-global virtio-mmio.force-legacy=false \
		-device virtio-gpu-device \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		-display default,show-cursor=off \
		-monitor none \
		-serial stdio \
		-kernel $(KERNEL)

# Crea un'immagine disco raw da 64MB.
# Se mkfs.ext4/mke2fs e' disponibile, la formatta gia' ext4 (4KB block size).
disk.img:
	@if [ ! -f disk.img ]; then \
		dd if=/dev/zero of=disk.img bs=1M count=64 2>/dev/null; \
		if command -v mkfs.ext4 >/dev/null 2>&1; then \
			mkfs.ext4 -F -q -b 4096 disk.img >/dev/null 2>&1; \
			echo "  disk.img creata e formattata ext4 (64MB, 4KB)"; \
		elif command -v mke2fs >/dev/null 2>&1; then \
			mke2fs -t ext4 -F -q -b 4096 disk.img >/dev/null 2>&1; \
			echo "  disk.img creata e formattata ext4 (64MB, 4KB)"; \
		else \
			echo "  disk.img creata raw (64MB): mkfs.ext4/mke2fs non trovato"; \
			echo "  Per M5-03 serve un'immagine ext4 valida su disk.img"; \
		fi; \
	else \
		echo "  disk.img gia' presente"; \
	fi

# Verifica che disk.img sia davvero ext4. Se non lo e', prova a formattarla.
disk-ready:
	@if [ ! -f disk.img ]; then \
		$(MAKE) disk.img; \
	else \
		magic=$$(dd if=disk.img bs=1 skip=1080 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n'); \
		if [ "$$magic" = "53ef" ]; then \
			echo "  disk.img valida: superblock ext4 trovato"; \
		else \
			echo "  disk.img presente ma non ext4: provo a riformattarla"; \
			if command -v mkfs.ext4 >/dev/null 2>&1; then \
				mkfs.ext4 -F -q -b 4096 disk.img >/dev/null 2>&1; \
				echo "  disk.img riformattata ext4 (64MB, 4KB)"; \
			elif command -v mke2fs >/dev/null 2>&1; then \
				mke2fs -t ext4 -F -q -b 4096 disk.img >/dev/null 2>&1; \
				echo "  disk.img riformattata ext4 (64MB, 4KB)"; \
			else \
				echo "  ERRORE: disk.img non e' ext4 e mkfs.ext4/mke2fs non e' disponibile"; \
				exit 1; \
			fi; \
		fi; \
	fi

disk-reset:
	@rm -f disk.img
	@$(MAKE) disk.img

disk-fsck: disk.img
	@if command -v e2fsck >/dev/null 2>&1; then \
		e2fsck -f disk.img; \
	else \
		echo "  e2fsck non trovato"; \
	fi

# Esegui con VirtIO-GPU + VirtIO-Block (M5-03 read-only ext4)
run-blk: $(KERNEL) disk-ready
	qemu-system-aarch64 \
		-machine virt \
		-accel tcg \
		-cpu cortex-a72 \
		-m 512M \
		-vga none \
		-global virtio-mmio.force-legacy=false \
		-device virtio-gpu-device \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		-drive format=raw,file=disk.img,if=none,id=blk0 \
		-device virtio-blk-device,drive=blk0 \
		-display default,show-cursor=off \
		-monitor none \
		-serial stdio \
		-kernel $(KERNEL)

# Debug con GDB
debug: $(KERNEL)
	qemu-system-aarch64 \
		-machine virt \
		-accel tcg \
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
