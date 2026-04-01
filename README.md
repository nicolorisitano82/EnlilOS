# EnlilOS - Realtime Operating System

Microkernel per AArch64 (ARMv8-A).

## Architettura

```
┌─────────────────────────────────────────────┐
│              User Space                      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐    │
│  │uart-server│ │fb-server │ │mem-server│    │
│  └─────┬────┘ └─────┬────┘ └─────┬────┘    │
│        │    IPC      │    IPC     │          │
├────────┴─────────────┴───────────┴──────────┤
│              EnlilOS Microkernel                │
│  ┌──────┐  ┌──────────┐  ┌──────────────┐  │
│  │ IPC  │  │Scheduler │  │Memory Manager│  │
│  └──────┘  └──────────┘  └──────────────┘  │
│  ┌──────────────────────────────────────┐   │
│  │         Port System (Mach-like)      │   │
│  └──────────────────────────────────────┘   │
├─────────────────────────────────────────────┤
│              Hardware (AArch64)              │
│         PL011 UART  │  Framebuffer          │
└─────────────────────────────────────────────┘
```

## Build

```bash
# Installa toolchain (macOS)
brew install aarch64-elf-gcc qemu

# Build
make

# Esegui (solo seriale)
make run

# Esegui con framebuffer grafico
make run-fb
```

## Struttura

```
nros/
├── boot/boot.S           # Codice di boot AArch64
├── kernel/
│   ├── main.c            # Entry point del kernel
│   └── microkernel.c     # Core: IPC, task, porte
├── drivers/
│   ├── uart.c            # Driver PL011 UART
│   └── framebuffer.c     # Driver framebuffer (ramfb)
├── include/
│   ├── types.h           # Tipi base
│   ├── uart.h            # Header UART
│   ├── framebuffer.h     # Header framebuffer
│   └── microkernel.h     # Header microkernel
├── linker.ld             # Linker script
└── Makefile              # Build system
```
