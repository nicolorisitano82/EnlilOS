# EnlilOS — Real-Time Microkernel

Microkernel real-time per AArch64 (ARMv8-A), target QEMU `virt` con CPU Cortex-A72.

Ispirato a GNU Hurd: task in kernel-space, IPC message-passing, server dedicati per ogni sottosistema hardware.

---

## Milestone completate

| ID | Milestone |
|----|-----------|
| M1-01 | Exception Vector Table — 16 handler, frame 288B, dump ESR_EL1 |
| M1-02 | MMU e Virtual Memory — block L1 1GB, WB cache, TLB prefault |
| M1-03 | Physical Memory Allocator — buddy 11 ordini + slab 7 classi |
| M1-04 | Kernel Heap — named typed caches (`task_cache`, `port_cache`, `ipc_cache`) |
| M2-01 | GIC-400 — tabella IRQ[256] O(1), priorità hardware |
| M2-02 | ARM Generic Timer — tick 1ms (1000 Hz), `timer_now_ns()` O(1) |
| M2-03 | Scheduler FPP — 256 priorità, ready bitmap, context switch in assembly |
| M3-01 | Syscall Dispatcher — tabella[256] O(1), ABI Linux AArch64 |
| M3-02 | Syscall Base — 13 syscall POSIX (write, read, exit, mmap, brk, waitpid…) |
| M3-03 | ANE Syscall Interface — 10 syscall Apple Neural Engine + SW fallback QEMU |
| M3-04 | GPU Syscall Interface — 14 syscall VirtIO-GPU / Apple AGX + SW fallback |
| M4-01 | PL050 PS/2 Keyboard — interrupt-driven, ring buffer SPSC 256B |
| M4-02 | VirtIO Input Keyboard — virtio-mmio, IRQ-driven |
| M4-03 | Terminal Line Discipline — echo, modalità canonica, backspace, CTRL+C |
| M4-04 | Font UTF-8 — decoder RFC 3629, font Latin-1 Supplement, API `fb_draw_*_utf8` |
| M4-05 | VirtIO Mouse — absolute/relative pointer, cursor guest, eventi click/wheel |
| M5-01 | VirtIO Block Device — virtio-blk, split vring, I/O sincrono bounded |

---

## Architettura

```
┌───────────────────────────────────────────────────────────┐
│                        User Space                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐ │
│  │blk-server│  │vfs-server│  │gpu-server│  │ane-server│ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘ │
│       │   IPC (message passing)    │              │       │
├───────┴────────────────────────────┴──────────────┴───────┤
│                    EnlilOS Microkernel                    │
│                                                           │
│  Scheduler FPP     GIC-400        ARM Generic Timer       │
│  256 priorità      IRQ[256] O(1)  tick 1ms, 1000Hz        │
│                                                           │
│  Syscall[256]      MMU AArch64    PMM Buddy+Slab          │
│  ABI Linux         identity map   O(1) hot path           │
│                                                           │
│  VirtIO MMIO bus (keyboard · mouse · GPU · blk)          │
│  ANE / Apple AGX (con SW fallback QEMU)                  │
├───────────────────────────────────────────────────────────┤
│               Hardware — QEMU virt AArch64                │
│  Cortex-A72  │  512MB RAM  │  PL011 UART  │  GIC-400     │
│  ramfb       │  VirtIO-GPU │  VirtIO-blk  │  PS/2 kbd    │
└───────────────────────────────────────────────────────────┘
```

---

## Build e avvio

### Requisiti

```bash
# macOS
brew install aarch64-elf-gcc qemu
```

### Compilazione

```bash
make          # produce enlil.elf + enlil.bin
```

### Target QEMU disponibili

| Comando | Descrizione |
|---------|-------------|
| `make run` | Solo UART seriale (no grafica) |
| `make run-fb` | UART + framebuffer ramfb |
| `make run-gpu` | UART + VirtIO-GPU + tastiera + mouse guest |
| `make run-blk` | Come `run-gpu` + VirtIO Block Device (`disk.img`) |
| `make debug` | GDB stub su porta 1234 |

```bash
# Crea immagine disco raw 64MB per run-blk
make disk.img
```

---

## Struttura del progetto

```
EnlilOS/
├── boot/
│   ├── boot.S              # Entry point AArch64, stack EL1, BSS zero
│   └── vectors.S           # Exception vector table (16 handler)
├── kernel/
│   ├── main.c              # kernel_main(): init sequenza + boot console
│   ├── microkernel.c       # IPC, task, porte (stile Mach/Hurd)
│   ├── exception.c         # Handler eccezioni + dispatcher syscall/IRQ
│   ├── mmu.c               # MMU AArch64, identity map, cache ops
│   ├── pmm.c               # Buddy allocator + slab allocator
│   ├── kheap.c             # Named typed caches (kmem_cache_t)
│   ├── gic.c               # GIC-400 driver, tabella IRQ O(1)
│   ├── timer.c             # ARM Generic Timer, tick 1ms
│   ├── sched.c             # Scheduler FPP 256 priorità
│   ├── sched_switch.S      # Context switch in assembly (x19-x30)
│   ├── syscall.c           # Dispatcher + 13 syscall base
│   ├── ane_syscall.c       # Syscall ANE (100-109)
│   ├── gpu_syscall.c       # Syscall GPU (120-133)
│   ├── utf8.c              # Decoder UTF-8 RFC 3629
│   ├── tty.c               # Line discipline (echo, canonical, CTRL+C)
│   └── vfs.c               # VFS layer (mount table, fd table)
├── drivers/
│   ├── uart.c              # PL011 UART
│   ├── keyboard.c          # VirtIO Input keyboard + PS/2 fallback
│   ├── mouse.c             # VirtIO Input mouse (abs/rel, cursor)
│   ├── blk.c               # VirtIO Block Device (I/O sincrono bounded)
│   ├── framebuffer.c       # ramfb + font bitmap + API UTF-8
│   ├── ane/
│   │   ├── ane_hw.c        # Backend ANE reale (Apple Silicon)
│   │   └── ane_sw.c        # SW fallback CPU (QEMU)
│   └── gpu/
│       ├── gpu_agx.c       # Backend Apple AGX
│       ├── gpu_sw.c        # SW fallback rasterizzazione CPU
│       └── gpu_virtio.c    # Backend VirtIO-GPU (QEMU)
├── include/                # Header pubblici di tutti i moduli
├── linker.ld               # Linker script (load @ 0x40080000)
├── Makefile
└── BACKLOG.md              # Roadmap dettagliata con design RT
```

---

## Principi real-time

| Vincolo | Regola |
|---------|--------|
| Latenza deterministica | Nessun ciclo WCET illimitato nei path critici |
| No demand paging | Tutta la memoria kernel pre-allocata al boot |
| No kmalloc in IRQ | Allocazione dinamica vietata negli handler |
| Priorità preemptiva | Un task ad alta priorità interrompe sempre |
| Priority Inheritance | Nessuna inversione su mutex e IPC |
| Static > dynamic | Pool statici per tutto ciò che è in hot path |
| I/O non diretto da RT | Task hard-RT comunicano col server blk via IPC con timeout |

---

## Prossime milestone

- **M5-02** — VFS Layer (server user-space, mount table, fd)
- **M5-03** — ext4 read path (superblock, extent tree, inode)
- **M5-04** — ext4 write path + journal
- **M5-05** — initrd CPIO bootstrap
