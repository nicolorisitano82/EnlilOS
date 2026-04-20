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
AR       = $(shell p="$$($(CC) -print-prog-name=ar 2>/dev/null)"; \
                   if [ -z "$$p" ] || [ "$$p" = "ar" ]; then printf '%s' "$(LD_PREFIX)ar"; else printf '%s' "$$p"; fi)
RANLIB   = $(shell p="$$($(CC) -print-prog-name=ranlib 2>/dev/null)"; \
                   if [ -z "$$p" ] || [ "$$p" = "ranlib" ]; then printf '%s' "$(LD_PREFIX)ranlib"; else printf '%s' "$$p"; fi)
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
           kernel/futex.c \
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
           kernel/mreact.c \
           kernel/ksem.c \
           kernel/kmon.c \
           kernel/cap.c \
           kernel/vmm.c \
           kernel/procfs.c \
           kernel/vfs.c \
           kernel/sock.c \
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
           drivers/net.c \
           drivers/blk.c \
           drivers/framebuffer.c

USER_STATIC_ASM_SRCS  = user/demo.S user/execve_demo.S user/execve_target.S
USER_STATIC_C_SRCS    = user/nsh.c user/fork_demo.c user/signal_demo.c user/mreact_demo.c user/cap_demo.c user/vfsd.c user/blkd.c user/netd.c user/mmap_demo.c user/job_demo.c user/ns_demo.c user/posix_demo.c user/musl_abi_demo.c user/tls_demo.c user/clone_demo.c user/thread_life_demo.c user/futex_demo.c
NETD_STACK_OBJ        = user/net_stack.o
USER_STATIC_OBJS      = $(USER_STATIC_ASM_SRCS:.S=.o) $(USER_STATIC_C_SRCS:.c=.o) \
                        $(NETD_STACK_OBJ)
USER_STATIC_ELFS      = $(USER_STATIC_ASM_SRCS:.S=.elf) $(USER_STATIC_C_SRCS:.c=.elf)
USER_STATIC_EMBEDOBJS = $(USER_STATIC_ASM_SRCS:.S=.embed.o) $(USER_STATIC_C_SRCS:.c=.embed.o)
USER_CRT_ASM_SRCS     = user/crti.S user/crtn.S
USER_CRT_C_SRCS       = user/crt1.c user/crt_demo.c
USER_CRT_OBJS         = $(USER_CRT_ASM_SRCS:.S=.o) $(USER_CRT_C_SRCS:.c=.o)
USER_CRT_ELFS         = user/crt_demo.elf
USER_CRT_EMBEDOBJS    = $(USER_CRT_ELFS:.elf=.embed.o)
USER_DYNAPP_SRCS      = user/dynamic_demo.c
USER_DYNAPP_PIEOBJS   = $(USER_DYNAPP_SRCS:.c=.pie.o)
USER_DYNAPP_ELFS      = $(USER_DYNAPP_SRCS:.c=.elf)
USER_DYNAPP_EMBEDOBJS = $(USER_DYNAPP_ELFS:.elf=.embed.o)
USER_SHARED_SRCS      = user/libdyn.c user/ld_enlil.c
USER_SHARED_PICOBJS   = $(USER_SHARED_SRCS:.c=.pic.o)
USER_SHARED_LIBS      = $(USER_SHARED_SRCS:.c=.so)
USER_SHARED_EMBEDOBJS = $(USER_SHARED_LIBS:%=%.embed.o)
USER_OBJS             = $(USER_STATIC_OBJS) $(USER_CRT_OBJS) $(USER_DYNAPP_PIEOBJS) $(USER_SHARED_PICOBJS)
USER_ELFS             = $(USER_STATIC_ELFS) $(USER_CRT_ELFS) $(USER_DYNAPP_ELFS) $(USER_SHARED_LIBS)
USER_EMBEDOBJS        = $(USER_STATIC_EMBEDOBJS) $(USER_CRT_EMBEDOBJS) $(USER_DYNAPP_EMBEDOBJS) $(USER_SHARED_EMBEDOBJS)
MUSL_ROOT             = toolchain/enlilos-musl
MUSL_BUILD            = toolchain/build/musl
MUSL_SYSROOT          = toolchain/sysroot
ARKSH_DIR            ?= $(CURDIR)/arksh
ARKSH_BUILD_DIR      ?= toolchain/build/arksh
ARKSH_COMPAT_DIR      = compat/arksh
ARKSH_TOOLCHAIN_FILE  = tools/enlilos-aarch64.cmake
ARKSH_SMOKE_DIR       = toolchain/cmake-smoke
ARKSH_SMOKE_BUILD     = toolchain/build/arksh-smoke
ARKSH_SMOKE_CACHE     = $(ARKSH_SMOKE_BUILD)/CMakeCache.txt
ARKSH_SMOKE_ELF       = $(ARKSH_SMOKE_BUILD)/arkshsmoke.elf
ARKSH_BOOT_ELF        = toolchain/smoke/arksh_boot.elf
ARKSH_SELFTEST_ELF    = toolchain/smoke/arksh_selftest.elf
ARKSH_REAL_ELF        = $(ARKSH_BUILD_DIR)/arksh
LINUX_COMPAT_STAGE_DIR ?= toolchain/build/linux-compat
LINUX_COMPAT_STAGE_MARK = $(LINUX_COMPAT_STAGE_DIR)/.staged
MUSL_SYSROOT_INC      = $(MUSL_SYSROOT)/usr/include
MUSL_SYSROOT_LIB      = $(MUSL_SYSROOT)/usr/lib
MUSL_WRAPPER_GCC      = toolchain/bin/aarch64-enlilos-musl-gcc
MUSL_WRAPPER_AR       = toolchain/bin/aarch64-enlilos-musl-ar
MUSL_WRAPPER_RANLIB   = toolchain/bin/aarch64-enlilos-musl-ranlib
MUSL_HEADER_SRCS      = $(MUSL_ROOT)/include/errno.h \
                        $(MUSL_ROOT)/include/ctype.h \
                        $(MUSL_ROOT)/include/dlfcn.h \
                        $(MUSL_ROOT)/include/dirent.h \
                        $(MUSL_ROOT)/include/fcntl.h \
                        $(MUSL_ROOT)/include/fnmatch.h \
                        $(MUSL_ROOT)/include/glob.h \
                        $(MUSL_ROOT)/include/math.h \
                        $(MUSL_ROOT)/include/pthread.h \
                        $(MUSL_ROOT)/include/regex.h \
                        $(MUSL_ROOT)/include/semaphore.h \
                        $(MUSL_ROOT)/include/signal.h \
                        $(MUSL_ROOT)/include/stdio.h \
                        $(MUSL_ROOT)/include/stdlib.h \
                        $(MUSL_ROOT)/include/string.h \
                        $(MUSL_ROOT)/include/time.h \
                        $(MUSL_ROOT)/include/termios.h \
                        $(MUSL_ROOT)/include/unistd.h \
                        $(MUSL_ROOT)/include/sys/ioctl.h \
                        $(MUSL_ROOT)/include/sys/resource.h \
                        $(MUSL_ROOT)/include/sys/mman.h \
                        $(MUSL_ROOT)/include/sys/stat.h \
                        $(MUSL_ROOT)/include/sys/time.h \
                        $(MUSL_ROOT)/include/sys/types.h \
                        $(MUSL_ROOT)/include/sys/uio.h \
                        $(MUSL_ROOT)/include/sys/utsname.h \
                        $(MUSL_ROOT)/include/sys/wait.h \
                        $(MUSL_ROOT)/include/sys/socket.h \
                        $(MUSL_ROOT)/include/netinet/in.h \
                        $(MUSL_ROOT)/include/arpa/inet.h
MUSL_HEADERS          = $(patsubst $(MUSL_ROOT)/include/%,$(MUSL_SYSROOT_INC)/%,$(MUSL_HEADER_SRCS))
MUSL_LIBC_SRCS        = $(MUSL_ROOT)/src/errno.c \
                        $(MUSL_ROOT)/src/ctype.c \
                        $(MUSL_ROOT)/src/dlfcn.c \
                        $(MUSL_ROOT)/src/dirent.c \
                        $(MUSL_ROOT)/src/fnmatch.c \
                        $(MUSL_ROOT)/src/glob.c \
                        $(MUSL_ROOT)/src/posix.c \
                        $(MUSL_ROOT)/src/pthread.c \
                        $(MUSL_ROOT)/src/regex.c \
                        $(MUSL_ROOT)/src/semaphore.c \
                        $(MUSL_ROOT)/src/string.c \
                        $(MUSL_ROOT)/src/stdlib.c \
                        $(MUSL_ROOT)/src/syscall.c \
                        $(MUSL_ROOT)/src/time.c \
                        $(MUSL_ROOT)/src/malloc.c \
                        $(MUSL_ROOT)/src/stdio.c \
                        $(MUSL_ROOT)/src/socket.c
MUSL_LIBC_OBJS        = $(patsubst $(MUSL_ROOT)/src/%.c,$(MUSL_BUILD)/libc/%.o,$(MUSL_LIBC_SRCS))
MUSL_LIBC_A           = $(MUSL_SYSROOT_LIB)/libc.a
MUSL_LIBDL_A          = $(MUSL_SYSROOT_LIB)/libdl.a
MUSL_SYSROOT_STAMP    = $(MUSL_BUILD)/sysroot.stamp
MUSL_SMOKE_SRCS       = toolchain/smoke/musl_hello.c \
                        toolchain/smoke/musl_stdio.c \
                        toolchain/smoke/musl_malloc.c \
                        toolchain/smoke/musl_fork_exec.c \
                        toolchain/smoke/musl_pipe_termios.c \
                        toolchain/smoke/musl_glob_fnmatch.c \
                        toolchain/smoke/musl_dlfcn.c \
                        toolchain/smoke/arksh_boot.c \
                        toolchain/smoke/musl_pthread.c \
                        toolchain/smoke/musl_semaphore.c \
                        toolchain/smoke/musl_tls_mt.c \
                        toolchain/smoke/loadkeys.c \
                        toolchain/smoke/kbdlayout.c \
                        toolchain/smoke/socket_demo.c \
                        toolchain/smoke/net_outbound.c
MUSL_SMOKE_ELFS       = $(MUSL_SMOKE_SRCS:.c=.elf)
ARKSH_CMAKE_FLAGS     = -DCMAKE_TOOLCHAIN_FILE=$(abspath $(ARKSH_TOOLCHAIN_FILE)) \
                        -DCMAKE_BUILD_TYPE=Release \
                        -DARKSH_STATIC=ON \
                        -DARKSH_PLUGINS=OFF \
                        -DENLILOS_COMPAT_DIR=$(abspath $(ARKSH_COMPAT_DIR))
ARKSH_REAL_INITRD     = $(if $(wildcard $(ARKSH_REAL_ELF)),usr/bin/arksh.real=$(ARKSH_REAL_ELF),)
LINUX_COMPAT_INITRD   = $(if $(wildcard $(LINUX_COMPAT_STAGE_MARK)),tree:sysroot=$(LINUX_COMPAT_STAGE_DIR),)
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

.PHONY: all clean run run-fb run-gpu run-blk debug dump disk-ready disk-reset disk-fsck test test-build musl-sysroot musl-smoke arksh-configure arksh-build arksh-smoke linux-compat-stage linux-compat-check

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

$(MUSL_SYSROOT_INC)/%: $(MUSL_ROOT)/include/%
	@mkdir -p $(dir $@)
	cp $< $@

$(MUSL_BUILD)/libc/%.o: $(MUSL_ROOT)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -mcpu=cortex-a72 -I$(MUSL_ROOT)/include -Iinclude -c $< -o $@

$(MUSL_SYSROOT_LIB)/crt1.o: user/crt1.o
	@mkdir -p $(MUSL_SYSROOT_LIB)
	cp $< $@

$(MUSL_SYSROOT_LIB)/crti.o: user/crti.o
	@mkdir -p $(MUSL_SYSROOT_LIB)
	cp $< $@

$(MUSL_SYSROOT_LIB)/crtn.o: user/crtn.o
	@mkdir -p $(MUSL_SYSROOT_LIB)
	cp $< $@

$(MUSL_LIBC_A): $(MUSL_LIBC_OBJS)
	@mkdir -p $(MUSL_SYSROOT_LIB)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(MUSL_LIBDL_A): $(MUSL_BUILD)/libc/dlfcn.o
	@mkdir -p $(MUSL_SYSROOT_LIB)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(MUSL_SYSROOT_STAMP): $(MUSL_HEADERS) $(MUSL_LIBC_A) \
                       $(MUSL_LIBDL_A) \
                       $(MUSL_SYSROOT_LIB)/crt1.o $(MUSL_SYSROOT_LIB)/crti.o $(MUSL_SYSROOT_LIB)/crtn.o
	@mkdir -p $(dir $@)
	@touch $@

$(ARKSH_SMOKE_CACHE): $(ARKSH_SMOKE_DIR)/CMakeLists.txt $(ARKSH_TOOLCHAIN_FILE) \
                      $(MUSL_SYSROOT_STAMP) $(MUSL_WRAPPER_GCC) Makefile
	env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS \
	    cmake -S $(ARKSH_SMOKE_DIR) -B $(ARKSH_SMOKE_BUILD) \
	          -DCMAKE_TOOLCHAIN_FILE=$(abspath $(ARKSH_TOOLCHAIN_FILE)) \
	          -DCMAKE_BUILD_TYPE=Release \
	          -DCMAKE_EXE_LINKER_FLAGS= \
	          -DCMAKE_MODULE_LINKER_FLAGS= \
	          -DCMAKE_SHARED_LINKER_FLAGS=

$(ARKSH_SMOKE_ELF): $(ARKSH_SMOKE_CACHE) $(ARKSH_SMOKE_DIR)/main.c \
                    $(MUSL_SYSROOT_STAMP) $(MUSL_WRAPPER_GCC) $(ARKSH_TOOLCHAIN_FILE)
	cmake --build $(ARKSH_SMOKE_BUILD) --target arkshsmoke

musl-sysroot: $(MUSL_SYSROOT_STAMP)
	@echo "Sysroot bootstrap pronta in $(MUSL_SYSROOT)"

toolchain/smoke/%.elf: toolchain/smoke/%.c $(MUSL_SYSROOT_STAMP) $(MUSL_WRAPPER_GCC)
	$(MUSL_WRAPPER_GCC) -O2 -o $@ $<

$(ARKSH_SELFTEST_ELF): toolchain/smoke/arksh_boot.c $(MUSL_SYSROOT_STAMP) $(MUSL_WRAPPER_GCC)
	$(MUSL_WRAPPER_GCC) -O2 -DARKSH_BOOT_SELFTEST -o $@ $<

musl-smoke: $(MUSL_SMOKE_ELFS)
	@echo "Smoke test buildati: $(MUSL_SMOKE_ELFS)"

arksh-configure: $(MUSL_SYSROOT_STAMP) $(MUSL_WRAPPER_GCC) $(ARKSH_TOOLCHAIN_FILE)
	@if [ ! -d "$(ARKSH_DIR)" ]; then \
		echo "arksh-configure: sorgente arksh non trovato in $(ARKSH_DIR)"; \
		echo "  imposta ARKSH_DIR=/percorso/arksh oppure clona il repo in ./arksh"; \
		exit 1; \
	fi
	env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS \
	    cmake -S "$(ARKSH_DIR)" -B "$(ARKSH_BUILD_DIR)" \
	          -DCMAKE_EXE_LINKER_FLAGS= \
	          -DCMAKE_MODULE_LINKER_FLAGS= \
	          -DCMAKE_SHARED_LINKER_FLAGS= \
	          $(ARKSH_CMAKE_FLAGS)

arksh-build: arksh-configure
	cmake --build "$(ARKSH_BUILD_DIR)" --target arksh

arksh-smoke: $(ARKSH_SMOKE_ELF)
	@echo "Smoke CMake/arksh pronta: $(ARKSH_SMOKE_ELF)"

linux-compat-stage:
	@if [ -z "$(LINUX_ROOT_DIR)" ]; then \
		echo "linux-compat-stage: imposta LINUX_ROOT_DIR=/percorso/root-linux-aarch64"; \
		exit 1; \
	fi
	python3 tools/stage_linux_compat.py "$(LINUX_ROOT_DIR)" "$(LINUX_COMPAT_STAGE_DIR)"

linux-compat-check:
	@if [ ! -f "$(LINUX_COMPAT_STAGE_MARK)" ]; then \
		echo "linux-compat-check: esegui prima 'make linux-compat-stage LINUX_ROOT_DIR=...'"; \
		exit 1; \
	fi
	@test -f "$(LINUX_COMPAT_STAGE_DIR)/usr/bin/bash" || (echo "manca usr/bin/bash" && exit 1)
	@test -f "$(LINUX_COMPAT_STAGE_DIR)/usr/bin/curl" || (echo "manca usr/bin/curl" && exit 1)
	@test -f "$(LINUX_COMPAT_STAGE_DIR)/usr/bin/env" || (echo "manca usr/bin/env" && exit 1)
	@test -f "$(LINUX_COMPAT_STAGE_DIR)/lib/ld-linux-aarch64.so.1" -o -f "$(LINUX_COMPAT_STAGE_DIR)/lib/ld-musl-aarch64.so.1" || \
		(echo "manca dynamic linker AArch64 compat" && exit 1)
	@echo "Linux compat staging OK in $(LINUX_COMPAT_STAGE_DIR)"

user/%.elf: user/%.o user/user.ld
	$(LD) -T user/user.ld -nostdlib -o $@ $<

# netd links net_stack.o alongside netd.o (M10-02)
user/netd.elf: user/netd.o $(NETD_STACK_OBJ) user/user.ld
	$(LD) -T user/user.ld -nostdlib -o $@ user/netd.o $(NETD_STACK_OBJ)

user/net_stack.o: user/net_stack.c user/net_stack.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/crt_demo.elf: user/crti.o user/crt1.o user/crt_demo.o user/crtn.o user/user.ld
	$(LD) -T user/user.ld -nostdlib -o $@ user/crti.o user/crt1.o user/crt_demo.o user/crtn.o

user/tls_demo.elf: user/crti.o user/crt1.o user/tls_demo.o user/crtn.o user/user.ld
	$(LD) -T user/user.ld -nostdlib -o $@ user/crti.o user/crt1.o user/tls_demo.o user/crtn.o

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

$(INITRD_CPIO): Makefile tools/mkinitrd.py initrd/README.TXT initrd/BOOT.TXT \
                initrd/vconsole.conf initrd/arkshrc initrd/arksh_user_rc \
                initrd/us.map initrd/it.map initrd/hostname initrd/hosts \
                initrd/passwd initrd/group initrd/os-release \
                initrd/nsswitch.conf initrd/ld.so.conf initrd/resolv.conf \
                $(USER_ELFS) $(USER_SHARED_LIBS) $(MUSL_SMOKE_ELFS) \
                $(ARKSH_SMOKE_ELF) $(ARKSH_SELFTEST_ELF) \
                $(wildcard $(ARKSH_REAL_ELF)) $(wildcard $(LINUX_COMPAT_STAGE_MARK))
	python3 tools/mkinitrd.py $@ \
		README.TXT=initrd/README.TXT \
		BOOT.TXT=initrd/BOOT.TXT \
		dir:bin \
		dir:dev \
		dir:data \
		dir:etc \
		dir:home \
		dir:home/user \
		dir:home/user/.config \
		dir:home/user/.config/arksh \
		dir:home/user/.local \
		dir:home/user/.local/state \
		dir:home/user/.local/state/arksh \
		dir:tmp \
		dir:var \
		dir:sysroot \
		dir:sysroot/usr \
		dir:sysroot/usr/bin \
		dir:sysroot/usr/share \
		dir:sysroot/usr/share/kbd \
		dir:sysroot/usr/share/kbd/keymaps \
		dir:usr \
		dir:usr/bin \
		dir:usr/share \
		dir:usr/share/kbd \
		dir:usr/share/kbd/keymaps \
		dir:usr/lib \
		dir:usr/lib/arksh \
		dir:usr/lib/arksh/plugins \
		INIT.ELF=$(ARKSH_BOOT_ELF) \
		DEMO.ELF=user/demo.elf \
		EXEC1.ELF=user/execve_demo.elf \
		EXEC2.ELF=user/execve_target.elf \
		DYNDEMO.ELF=user/dynamic_demo.elf \
		FORKDEMO.ELF=user/fork_demo.elf \
		SIGDEMO.ELF=user/signal_demo.elf \
		MREACTDEMO.ELF=user/mreact_demo.elf \
		CAPDEMO.ELF=user/cap_demo.elf \
		VFSD.ELF=user/vfsd.elf \
		BLKD.ELF=user/blkd.elf \
		NETD.ELF=user/netd.elf \
		MMAPDEMO.ELF=user/mmap_demo.elf \
		JOBDEMO.ELF=user/job_demo.elf \
		NSDEMO.ELF=user/ns_demo.elf \
		POSIXDEMO.ELF=user/posix_demo.elf \
		MUSLABI.ELF=user/musl_abi_demo.elf \
		TLSDEMO.ELF=user/tls_demo.elf \
		CLONEDEMO.ELF=user/clone_demo.elf \
		THREADLIFE.ELF=user/thread_life_demo.elf \
		FUTEXDEMO.ELF=user/futex_demo.elf \
		CRTDEMO.ELF=user/crt_demo.elf \
		MUSLHELLO.ELF=toolchain/smoke/musl_hello.elf \
		MUSLSTDIO.ELF=toolchain/smoke/musl_stdio.elf \
		MUSLMALLOC.ELF=toolchain/smoke/musl_malloc.elf \
		MUSLFORK.ELF=toolchain/smoke/musl_fork_exec.elf \
		MUSLPIPE.ELF=toolchain/smoke/musl_pipe_termios.elf \
		MUSLGLOB.ELF=toolchain/smoke/musl_glob_fnmatch.elf \
		MUSLDL.ELF=toolchain/smoke/musl_dlfcn.elf \
		LOADKEYS.ELF=toolchain/smoke/loadkeys.elf \
		KBDLAYOUT.ELF=toolchain/smoke/kbdlayout.elf \
		PTHREADDEMO.ELF=toolchain/smoke/musl_pthread.elf \
		SEMDEMO.ELF=toolchain/smoke/musl_semaphore.elf \
		TLSMTDEMO.ELF=toolchain/smoke/musl_tls_mt.elf \
		SOCKDEMO.ELF=toolchain/smoke/socket_demo.elf \
		NETOUT.ELF=toolchain/smoke/net_outbound.elf \
		ARKSHBOOT.ELF=$(ARKSH_SELFTEST_ELF) \
		ARKSHSMK.ELF=$(ARKSH_SMOKE_ELF) \
		bin/arksh=$(ARKSH_BOOT_ELF) \
		bin/nsh=user/nsh.elf \
		$(ARKSH_REAL_INITRD) \
		usr/bin/loadkeys=toolchain/smoke/loadkeys.elf \
		usr/bin/kbdlayout=toolchain/smoke/kbdlayout.elf \
		sysroot/usr/bin/loadkeys=toolchain/smoke/loadkeys.elf \
		sysroot/usr/bin/kbdlayout=toolchain/smoke/kbdlayout.elf \
		usr/share/kbd/keymaps/us.map=initrd/us.map \
		usr/share/kbd/keymaps/it.map=initrd/it.map \
		sysroot/usr/share/kbd/keymaps/us.map=initrd/us.map \
		sysroot/usr/share/kbd/keymaps/it.map=initrd/it.map \
		etc/vconsole.conf=initrd/vconsole.conf \
		etc/hostname=initrd/hostname \
		etc/hosts=initrd/hosts \
		etc/passwd=initrd/passwd \
		etc/group=initrd/group \
		etc/os-release=initrd/os-release \
		etc/nsswitch.conf=initrd/nsswitch.conf \
		etc/ld.so.conf=initrd/ld.so.conf \
		etc/resolv.conf=initrd/resolv.conf \
		etc/arkshrc=initrd/arkshrc \
		home/user/.config/arksh/arkshrc=initrd/arksh_user_rc \
		libdyn.so=user/libdyn.so \
		LD-ENLIL.SO=user/ld_enlil.so \
		$(LINUX_COMPAT_INITRD) NSH.ELF=user/nsh.elf

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
		-netdev user,id=net0 \
		-device virtio-net-device,netdev=net0 \
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
		-netdev user,id=net0 \
		-device virtio-net-device,netdev=net0 \
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
		-netdev user,id=net0 \
		-device virtio-net-device,netdev=net0 \
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
		-netdev user,id=net0 \
		-device virtio-net-device,netdev=net0 \
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
		-netdev user,id=net0 \
		-device virtio-net-device,netdev=net0 \
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
	      $(KSYMS_DATA) $(KSYMS_SELFTEST_DATA) $(MUSL_SMOKE_ELFS)
	rm -rf $(MUSL_BUILD) $(MUSL_SYSROOT) $(ARKSH_BUILD_DIR) $(ARKSH_SMOKE_BUILD)
	@echo "Clean completato."
