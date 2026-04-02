#
# EnlilOS Microkernel - Build System
# Target: AArch64 (ARMv8-A) - QEMU virt machine
#

# Toolchain - usa il cross-compiler AArch64
# Su macOS: brew install aarch64-elf-gcc
# Su Linux: apt install gcc-aarch64-linux-gnu
ifeq ($(origin CROSS), undefined)
ifeq ($(shell command -v aarch64-elf-gcc >/dev/null 2>&1; echo $$?),0)
CROSS := aarch64-elf-
else ifeq ($(shell command -v aarch64-none-elf-gcc >/dev/null 2>&1; echo $$?),0)
CROSS := aarch64-none-elf-
else ifeq ($(shell command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; echo $$?),0)
CROSS := aarch64-linux-gnu-
else
CROSS := aarch64-elf-
endif
endif
CC       = $(CROSS)gcc
AS       = $(CC)
LD       = $(shell $(CC) -print-prog-name=ld 2>/dev/null || printf '%s' "$(CROSS)ld")
LD_PREFIX = $(patsubst %ld,%,$(LD))
OBJCOPY  = $(shell p="$$($(CC) -print-prog-name=objcopy 2>/dev/null)"; \
                   if [ -z "$$p" ] || [ "$$p" = "objcopy" ]; then printf '%s' "$(LD_PREFIX)objcopy"; else printf '%s' "$$p"; fi)
OBJDUMP  = $(shell p="$$($(CC) -print-prog-name=objdump 2>/dev/null)"; \
                   if [ -z "$$p" ] || [ "$$p" = "objdump" ]; then printf '%s' "$(LD_PREFIX)objdump"; else printf '%s' "$$p"; fi)
NM       = $(shell p="$$($(CC) -print-prog-name=nm 2>/dev/null)"; \
                   if [ -z "$$p" ] || [ "$$p" = "nm" ]; then printf '%s' "$(LD_PREFIX)nm"; else printf '%s' "$$p"; fi)

# Override manuale:
#   make CROSS=aarch64-none-elf-
#   make CROSS=/path/to/bin/aarch64-none-elf-

# Flags
CFLAGS   = -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
           -fno-omit-frame-pointer \
           -mcpu=cortex-a72 -Iinclude -Idrivers/ane -Idrivers/gpu
ASFLAGS  = -mcpu=cortex-a72
LDFLAGS  = -T linker.ld -nostdlib

# Sorgenti
ASM_SRCS = boot/boot.S \
           boot/vectors.S \
           kernel/sched_switch.S
C_SRCS   = kernel/main.c \
           kernel/initrd.c \
           kernel/elf_loader.c \
           kernel/kdebug.c \
           kernel/signal.c \
           kernel/microkernel.c \
           kernel/exception.c \
           kernel/mmu.c \
           kernel/pmm.c \
           kernel/kheap.c \
           kernel/gic.c \
           kernel/timer.c \
           kernel/sched.c \
           kernel/tty.c \
           kernel/term80.c \
           kernel/string.c \
           kernel/selftest.c \
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

USER_STATIC_ASM_SRCS  = user/demo.S user/execve_demo.S user/execve_target.S
USER_STATIC_C_SRCS    = user/nsh.c user/fork_demo.c user/signal_demo.c
USER_STATIC_OBJS      = $(USER_STATIC_ASM_SRCS:.S=.o) $(USER_STATIC_C_SRCS:.c=.o)
USER_STATIC_ELFS      = $(USER_STATIC_ASM_SRCS:.S=.elf) $(USER_STATIC_C_SRCS:.c=.elf)
USER_STATIC_EMBEDOBJS = $(USER_STATIC_ASM_SRCS:.S=.embed.o) $(USER_STATIC_C_SRCS:.c=.embed.o)
USER_DYNAPP_SRCS      = user/dynamic_demo.c
USER_DYNAPP_PIEOBJS   = $(USER_DYNAPP_SRCS:.c=.pie.o)
USER_DYNAPP_ELFS      = $(USER_DYNAPP_SRCS:.c=.elf)
USER_DYNAPP_EMBEDOBJS = $(USER_DYNAPP_ELFS:.elf=.embed.o)
USER_SHARED_SRCS      = user/libdyn.c user/ld_enlil.c
USER_SHARED_PICOBJS   = $(USER_SHARED_SRCS:.c=.pic.o)
USER_SHARED_LIBS      = $(USER_SHARED_SRCS:.c=.so)
USER_SHARED_EMBEDOBJS = $(USER_SHARED_LIBS:%=%.embed.o)
USER_OBJS             = $(USER_STATIC_OBJS) $(USER_DYNAPP_PIEOBJS) $(USER_SHARED_PICOBJS)
USER_ELFS             = $(USER_STATIC_ELFS) $(USER_DYNAPP_ELFS) $(USER_SHARED_LIBS)
USER_EMBEDOBJS        = $(USER_STATIC_EMBEDOBJS) $(USER_DYNAPP_EMBEDOBJS) $(USER_SHARED_EMBEDOBJS)
INITRD_CPIO           = boot/initrd.cpio
INITRD_EMBEDOBJ       = boot/initrd.embed.o
KSYMS_DATA            = kernel/ksyms_data.c
KSYMS_SELFTEST_DATA   = kernel/ksyms_data.selftest.c
KSYMS_OBJ             = kernel/ksyms_data.o
KSYMS_SELFTEST_OBJ    = kernel/ksyms_data.selftest.o
KSYMS_STUB_OBJ        = kernel/ksyms_stub.o

USER_CFLAGS      = -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
                   -fno-builtin -mcpu=cortex-a72 -Iinclude
USER_PIC_CFLAGS  = $(USER_CFLAGS) -fPIC
USER_PIE_CFLAGS  = $(USER_CFLAGS) -fPIE

# Oggetti
BASE_OBJS         = $(ASM_SRCS:.S=.o) $(C_SRCS:.c=.o) $(INITRD_EMBEDOBJ) $(USER_EMBEDOBJS)
BASE_SELFTEST_OBJS = $(ASM_SRCS:.S=.selftest.o) $(C_SRCS:.c=.selftest.o) $(INITRD_EMBEDOBJ) $(USER_EMBEDOBJS)
OBJS              = $(BASE_OBJS) $(KSYMS_OBJ)
SELFTEST_OBJS     = $(BASE_SELFTEST_OBJS) $(KSYMS_SELFTEST_OBJ)

# Output
KERNEL   = enlil.elf
KERNEL_BIN = enlil.bin
SELFTEST_KERNEL = enlil-selftest.elf
PASS1_KERNEL = enlil.pass1.elf
PASS1_SELFTEST_KERNEL = enlil-selftest.pass1.elf

# === Targets ===

.PHONY: all clean run run-fb run-gpu run-blk debug dump disk-ready disk-reset disk-fsck test test-build

all: $(KERNEL) $(KERNEL_BIN)
	@echo ""
	@echo "  ╔══════════════════════════════════════╗"
	@echo "  ║  EnlilOS Microkernel - Build completata ║"
	@echo "  ║  Output: $(KERNEL)                ║"
	@echo "  ╚══════════════════════════════════════╝"
	@echo ""

$(PASS1_KERNEL): $(BASE_OBJS) $(KSYMS_STUB_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

$(KSYMS_DATA): tools/gen_ksyms.py $(PASS1_KERNEL)
	NM="$(NM)" python3 tools/gen_ksyms.py $(PASS1_KERNEL) $@

$(KSYMS_OBJ): $(KSYMS_DATA)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(PASS1_KERNEL) $(KSYMS_OBJ)
	$(LD) $(LDFLAGS) -o $@ $(BASE_OBJS) $(KSYMS_OBJ)

$(KERNEL_BIN): $(KERNEL)
	$(OBJCOPY) -O binary $< $@

$(PASS1_SELFTEST_KERNEL): $(BASE_SELFTEST_OBJS) $(KSYMS_STUB_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

$(KSYMS_SELFTEST_DATA): tools/gen_ksyms.py $(PASS1_SELFTEST_KERNEL)
	NM="$(NM)" python3 tools/gen_ksyms.py $(PASS1_SELFTEST_KERNEL) $@

$(KSYMS_SELFTEST_OBJ): $(KSYMS_SELFTEST_DATA)
	$(CC) $(CFLAGS) -DENLILOS_SELFTEST=1 -c $< -o $@

$(SELFTEST_KERNEL): $(PASS1_SELFTEST_KERNEL) $(KSYMS_SELFTEST_OBJ)
	$(LD) $(LDFLAGS) -o $@ $(BASE_SELFTEST_OBJS) $(KSYMS_SELFTEST_OBJ)

user/%.elf: user/%.o user/user.ld
	$(LD) -T user/user.ld -nostdlib -o $@ $<

user/%.o: user/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/%.pic.o: user/%.c
	$(CC) $(USER_PIC_CFLAGS) -c $< -o $@

user/%.pie.o: user/%.c
	$(CC) $(USER_PIE_CFLAGS) -c $< -o $@

user/%.so: user/%.pic.o user/user_shared.ld
	$(CC) -shared -nostdlib -Wl,-T,user/user_shared.ld -o $@ $<

user/dynamic_demo.elf: user/dynamic_demo.pie.o user/user_dyn.ld user/libdyn.so
	$(CC) -pie -nostdlib -Wl,-T,user/user_dyn.ld \
	      -Wl,--dynamic-linker=/LD-ENLIL.SO -Luser -ldyn -o $@ $<

$(INITRD_CPIO): tools/mkinitrd.py initrd/README.TXT initrd/BOOT.TXT $(USER_ELFS) $(USER_SHARED_LIBS)
	python3 tools/mkinitrd.py $@ \
		README.TXT=initrd/README.TXT \
		BOOT.TXT=initrd/BOOT.TXT \
		dir:dev \
		dir:data \
		dir:sysroot \
		INIT.ELF=user/nsh.elf \
		DEMO.ELF=user/demo.elf \
		EXEC1.ELF=user/execve_demo.elf \
		EXEC2.ELF=user/execve_target.elf \
		DYNDEMO.ELF=user/dynamic_demo.elf \
		FORKDEMO.ELF=user/fork_demo.elf \
		SIGDEMO.ELF=user/signal_demo.elf \
		libdyn.so=user/libdyn.so \
		LD-ENLIL.SO=user/ld_enlil.so \
		NSH.ELF=user/nsh.elf

$(INITRD_EMBEDOBJ): $(INITRD_CPIO)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

%.embed.o: %.elf
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

%.so.embed.o: %.so
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

%.o: %.S
	$(AS) $(ASFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.selftest.o: %.S
	$(AS) $(ASFLAGS) -DENLILOS_SELFTEST=1 -c $< -o $@

%.selftest.o: %.c
	$(CC) $(CFLAGS) -DENLILOS_SELFTEST=1 -c $< -o $@

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

# Esegui con VirtIO-GPU + VirtIO-Block (M5-03 / M5-04 ext4 rw-core)
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

test-build: $(SELFTEST_KERNEL)
	@echo "Selftest kernel pronto: $(SELFTEST_KERNEL)"

test: $(SELFTEST_KERNEL) disk-ready
	qemu-system-aarch64 \
		-machine virt \
		-accel tcg \
		-cpu cortex-a72 \
		-m 512M \
		-vga none \
		-global virtio-mmio.force-legacy=false \
		-device virtio-gpu-device \
		-drive format=raw,file=disk.img,if=none,id=blk0 \
		-device virtio-blk-device,drive=blk0 \
		-display none \
		-monitor none \
		-serial stdio \
		-kernel $(SELFTEST_KERNEL)

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
	rm -f $(OBJS) $(SELFTEST_OBJS) $(KERNEL) $(KERNEL_BIN) $(SELFTEST_KERNEL) \
	      $(PASS1_KERNEL) $(PASS1_SELFTEST_KERNEL) \
	      $(USER_OBJS) $(USER_ELFS) $(USER_EMBEDOBJS) $(INITRD_CPIO) \
	      $(KSYMS_DATA) $(KSYMS_SELFTEST_DATA)
	@echo "Clean completato."
