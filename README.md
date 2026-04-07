# EnlilOS — Real-Time Microkernel

Microkernel real-time per AArch64 (ARMv8-A), sviluppato e testato soprattutto su QEMU `virt` con CPU `cortex-a72`.

Il progetto combina vincoli di affidabilita' e latenza prevedibile con funzionalita' da sistema operativo general-purpose: scheduler a priorita' fisse, IPC sincrono, rootfs `initrd` embedded, `ext4` su `virtio-blk`, loader ELF64 per task EL0, shell utente e backend grafici `ramfb` / `virtio-gpu`.

---

## Stato attuale

Le milestone completate oggi coprono:

- **M1-M4**: boot AArch64, exception handling, MMU, PMM buddy+slab, heap kernel, GIC-400, timer, scheduler FPP, syscall base, input tastiera/mouse, TTY line discipline e font UTF-8.
- **M5**: `virtio-blk`, VFS bootstrap, `ext4` read/write con journal bounded e recovery al mount, rootfs `/` come `initrd` `CPIO newc` embedded, mount `/data` e `/sysroot`.
- **M5b**: backend GPU `virtio-gpu` / `ramfb`, scanout, memory manager GPU, renderer 2D e boot graphics console.
- **M6**: loader ELF64 statico e dinamico per task EL0, `execve()`, shared object bootstrap e demo userspace.
- **M7**: IPC sincrono stile microkernel con donation/budget e shell userspace `NSH`.

Il backlog principale `BACKLOG.md` e' chiuso: il selftest QEMU corrente passa con
`SUMMARY total=8 pass=8 fail=0`.

---

## Architettura

Per una vista completa dell'architettura target del sistema, basata su `BACKLOG.md`,
`BACKLOG2.md` e `BACKLOG3.md`, vedi [ARCHITETTURA_TARGET_ENLILOS.md](docs/ARCHITETTURA_TARGET_ENLILOS.md).

```text
+---------------------------------------------------------------+
|                           User Space                          |
|   ELF64 statici/dinamici, NSH, demo execve, demo shared-lib   |
+---------------------------+-----------------------------------+
|      IPC sync / ports     |  spawn/execve / VFS / GPU / TTY   |
+---------------------------+-----------------------------------+
|                    EnlilOS Microkernel                        |
|                                                               |
| Scheduler FPP | Syscall table | MMU | PMM | Kheap | Timer     |
| GIC-400       | IPC sync      | VFS | ext4 | GPU | input      |
|                                                               |
| Device model: PL011, VirtIO input, VirtIO-blk, VirtIO-GPU     |
+---------------------------------------------------------------+
|                    QEMU virt / AArch64                        |
| cortex-a72 | 512MB RAM | GIC-400 | ramfb | virtio-mmio        |
+---------------------------------------------------------------+
```

Nota pratica: il progetto oggi e' ancora in una fase di bootstrap ibrida. Alcuni server sono modellati in stile microkernel, ma parte della logica gira ancora in-kernel per accelerare il bring-up e mantenere il debugging semplice.

---

## Root filesystem e mount

Al boot, la mount table osservabile oggi e':

- `/` -> `initrd-cpio` read-only embedded nel kernel
- `/dev` -> `devfs`
- `/data` -> `ext4` su `virtio-blk` quando il disco e' presente
- `/sysroot` -> `ext4` su `virtio-blk` quando il disco e' presente

L'`initrd` contiene almeno:

- `README.TXT`
- `BOOT.TXT`
- `INIT.ELF`
- `NSH.ELF`
- `DEMO.ELF`
- `EXEC1.ELF`
- `EXEC2.ELF`
- `DYNDEMO.ELF`
- `libdyn.so`
- `LD-ENLIL.SO`

Questa scelta permette un bootstrap completamente in RAM e accessi O(1) ai file iniziali, utile per configurazione iniziale, recovery e bootstrap dei primi task user-space.

---

## Build e dipendenze

### Requisiti

```bash
# macOS
brew install aarch64-elf-gcc qemu

# opzionale ma consigliato per run-blk / ext4
brew install e2fsprogs
```

Se `mkfs.ext4` non e' nel `PATH`, su macOS Homebrew si trova tipicamente in:

```bash
$(brew --prefix e2fsprogs)/sbin/mkfs.ext4
```

### Compilazione

```bash
make
```

Output principali:

- `enlil.elf`
- `enlil.bin`
- `boot/initrd.cpio`

Il `Makefile` supporta anche prefissi espliciti:

```bash
make CROSS=/opt/homebrew/Cellar/aarch64-elf-gcc/15.2.0/bin/aarch64-elf-
```

---

## Target QEMU

| Comando | Descrizione |
|---------|-------------|
| `make run` | solo seriale PL011 |
| `make run-fb` | seriale + `ramfb` |
| `make run-gpu` | seriale + `virtio-gpu` + tastiera + mouse |
| `make run-blk` | come `run-gpu` + `virtio-blk` con `disk.img` |
| `make test` | avvio del kernel selftest sotto QEMU |
| `make debug` | QEMU in attesa di GDB sulla porta 1234 |

Target utili per il disco:

| Comando | Descrizione |
|---------|-------------|
| `make disk.img` | crea `disk.img` da 64MB |
| `make disk-ready` | verifica che `disk.img` sia davvero `ext4` |
| `make disk-reset` | ricrea l'immagine disco |
| `make disk-fsck` | esegue `e2fsck` se disponibile |

---

## Console di boot e shell

La boot console supporta sia seriale sia modalita' grafica. Alcuni comandi utili:

- `help`
- `pwd`
- `cd /data`
- `ls`
- `cat /BOOT.TXT`
- `write`, `append`, `truncate`, `sync`, `fsync`
- `mkdir`, `rm`, `mv`
- `elfdemo`, `execdemo`, `dyndemo`, `runelf PATH`
- `nsh`
- `selftest`

`NSH` e' una shell EL0 minimale integrata nel rootfs bootstrap. Al momento espone:

- `ls`
- `cat`
- `echo`
- `exec`
- `clear`
- `top`
- `cd`
- `pwd`
- `help`
- `exit`

---

## Test

Esiste una suite di self-test kernel-side che verifica i sottosistemi piu' recenti, tra cui:

- `vfs-rootfs`
- `vfs-devfs`
- `ext4-core`
- `elf-loader`
- `execve`
- `elf-dynamic`
- `ipc-sync`
- `gpu-stack`

La build dedicata e':

```bash
make test-build
make test
```

Nota: se il selftest si blocca, conviene leggere il log seriale completo. La suite e' pensata per isolare regressioni su mount, exec, IPC e stack grafico.

---

## Struttura del progetto

```text
EnlilOS/
├── boot/                  # entry point, vectors, initrd embedded
├── drivers/
│   ├── uart.c            # PL011
│   ├── keyboard.c        # VirtIO keyboard + fallback UART
│   ├── mouse.c           # VirtIO mouse
│   ├── blk.c             # VirtIO block
│   ├── framebuffer.c     # ramfb + font bitmap
│   ├── gpu/              # virtio-gpu, AGX, SW backend
│   └── ane/              # Apple Neural Engine / fallback CPU
├── include/              # header pubblici
├── initrd/               # contenuti statici dell'initrd embedded
├── kernel/
│   ├── main.c            # boot sequence + boot console
│   ├── initrd.c          # parser CPIO read-only
│   ├── vfs.c             # mount table + dispatch VFS
│   ├── ext4.c            # ext4 read/write core
│   ├── elf_loader.c      # ELF64 static/dynamic loader
│   ├── microkernel.c     # IPC / ports / task model
│   ├── sched.c           # scheduler FPP
│   ├── tty.c             # line discipline
│   ├── term80.c          # terminale testuale 80x25
│   └── selftest.c        # suite integrata
├── tools/
│   └── mkinitrd.py       # builder host-side dell'archivio CPIO
├── user/                 # ELF userspace statici e dinamici
├── BACKLOG.md            # roadmap principale
└── BACKLOG2.md           # roadmap estesa / extra milestone
```

---

## Principi real-time

| Vincolo | Regola |
|---------|--------|
| Latenza prevedibile | evitare path senza bound chiaro nel kernel |
| No allocazioni in IRQ | hot path e interrupt usano pool o strutture statiche |
| Priority-driven | task ad alta priorita' preemptano sempre quelli piu' bassi |
| IPC bounded | rendez-vous con budget e metriche per-porta |
| Bootstrap in RAM | `initrd` embedded per accesso immediato a file e binari base |
| I/O isolato | task hard-RT non dipendono da I/O bloccante diretto |

---

## Stato roadmap

Se guardi il backlog principale, il nucleo del sistema e' gia' oltre il bring-up:

- storage e rootfs bootstrap sono presenti
- userspace EL0 con loader dinamico e `execve()` e' presente
- shell `NSH` e IPC sincrono sono presenti

Il path core descritto in [BACKLOG.md](BACKLOG.md) e' completato; il lavoro successivo continua nella roadmap estesa in [BACKLOG2.md](BACKLOG2.md).
