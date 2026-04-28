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
- **M8**: `fork()` con Copy-on-Write, `mmap()` file-backed con `msync()/munmap()`, signal handling, process groups/sessioni/job control, `pipe/dup/dup2`, `getcwd/chdir`, `termios/isatty`, `glob()/fnmatch()` bootstrap user-space, build/toolchain CMake `v1` per `arksh`, integrazione login shell `v1` con `/bin/arksh`, binario reale esterno `/bin/arksh.real` quando disponibile, fallback `/bin/nsh`, plugin dinamici `arksh` via `dlopen()` (`/usr/lib/arksh/plugins/enlil.so`, smoke `ARKSHPLUGIN.ELF`), `mreact`, `ksem`, `kmon` e layout tastiera multipli `us`/`it` con `loadkeys`, `kbdlayout` e persistenza via `vconsole.conf`.
- **M9**: capability kernel-side, `vfsd` e `blkd` user-space bootstrap via IPC, mount dinamico, namespace privati, bind mount e `pivot_root()`.
- **M10**: driver `virtio-net` MMIO `v1`, `netd` bootstrap con stack IPv4/ARP/ICMP/UDP/TCP minimale e BSD socket API `v1` (`AF_INET`, `SOCK_STREAM`/`SOCK_DGRAM`) loopback-only su `127.0.0.1`.
- **M11-01**: bootstrap musl/toolchain `v1` con ABI minima (`getpid/getppid/gettimeofday/nanosleep`, uid/gid stub, `lseek`, `readv/writev`, `fcntl`, `openat`, `fstatat`, `ioctl`, `uname`), TLS statico (`PT_TLS`, `TPIDR_EL0`, `AT_RANDOM`/uid/gid), runtime `crt1/crti/crtn`, sysroot `usr/include` + `libc.a`, wrapper `aarch64-enlilos-musl-*` e smoke test `hello`, `stdio`, `malloc`, `fork-exec`, `pipe-termios`.
- **M11-02a/b/c/d/e**: profilo multi-thread `v1` chiuso, con `tgid/gettid`, `clone()` subset thread-oriented, stato processo condiviso (`mm/files/sighand/fs`) via `proc_slot`, `set_tid_address()`, `exit_group()`, `tgkill()`, `futex` (`WAIT/WAKE/REQUEUE/CMP_REQUEUE`), wake su `clear_child_tid`, wrapper musl `pthread`/`sem_t`, `pthread_mutex/cond`, TLS statico multi-thread per `__thread`, `errno` thread-local e smoke `musl-pthread` + `musl-sem` + `tls-mt`.
- **M11-03**: dynamic linking `v1` con `dlopen/dlsym/dlclose/dlerror`, load runtime di `ET_DYN`, risoluzione `DT_NEEDED` e smoke `musl-dlfcn`.
- **M11-08**: application bundle `.enlil` `v1` con parser TOON bootstrap, launcher `/bin/enlil-run`, `ENLIL_BUNDLE_ROOT`, ricerca librerie bundle-local e demo `hello.enlil`.
- **M11-05a/b/c**: compat Linux AArch64 `v1` con tabella syscall separata, mount compat (`/lib`, `/usr`, `/bin/sh`, `/proc`, `/dev`, `/etc`), supporto `ET_EXEC` low-VA tramite alias user-space, `bash-linux` statico funzionante da `/data/bash-linux`, `epoll_create1/ctl/pwait` bounded e System V IPC `v1` (`shmget/shmat/shmdt/shmctl`, `semget/semop/semctl`).
- **M11-05d**: shim `PT_INTERP` — `ld-linux-aarch64.so.1` risolto automaticamente come alias verso `/LD-ENLIL.SO` quando il file non esiste nel VFS; search path `DT_NEEDED` esteso a `/lib/aarch64-linux-gnu`, `/usr/lib`.
- **M11-05e**: Linux filesystem environment — `/proc/sys` subtree, `/etc/locale.conf`, `/etc/ld.so.cache` stub, `/etc/localtime` TZif2 UTC, `/proc/<pid>/maps` stub.
- **M11-05f**: PTY pseudo-terminal `v1` — `posix_openpt/grantpt/unlockpt/ptsname_r`, `/dev/ptmx` e `/dev/pts/N`, line discipline completa (ICANON, ECHO, ISIG, ICRNL, OPOST/ONLCR), `TIOCGWINSZ/TIOCSWINSZ`, `SIGWINCH` (default ignore POSIX), musl bootstrap con `openpty`.
- **M14**: `procfs` core montato su `/proc` e crash reporter con stack trace simbolico.

Il backlog principale `BACKLOG.md` e' chiuso e il backlog esteso `BACKLOG2.md`
ha diverse milestone reali implementate. Il selftest QEMU corrente passa con
`SUMMARY total=60 pass=60 fail=0`.

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
| Device model: PL011, VirtIO input, VirtIO-blk, VirtIO-GPU,    |
| VirtIO-net                                                    |
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
- `/dev` -> `devfs` (include `/dev/ptmx`, `/dev/pts/N`)
- `/proc` -> `procfs` read-only
- `/data` -> `ext4` su `virtio-blk` quando il disco e' presente
- `/sysroot` -> `ext4` su `virtio-blk` quando il disco e' presente

L'`initrd` e' generato a build-time e contiene almeno:

- `README.TXT`, `BOOT.TXT`
- `dev/`, `data/`, `sysroot/`
- `INIT.ELF`, `NSH.ELF`
- `ARKSHBOOT.ELF`
- `ENLILRUN.ELF`
- `ARKSHPLUGIN.ELF`
- `bin/arksh`, `bin/arksh.real` (se build esterna presente), `bin/nsh`
- `bin/enlil-run`
- `usr/lib/arksh/plugins/enlil.so`
- `usr/bin/loadkeys`, `usr/bin/kbdlayout`
- `etc/arkshrc`, `etc/vconsole.conf`, `etc/locale.conf`, `etc/localtime`, `etc/ld.so.cache`
- `home/user/.config/arksh/arkshrc`
- `usr/share/kbd/keymaps/us.map`, `usr/share/kbd/keymaps/it.map`
- ELF demo: `DEMO.ELF`, `EXEC1.ELF`, `EXEC2.ELF`, `DYNDEMO.ELF`, `LDINTDEMO.ELF`
- ELF demo: `FORKDEMO.ELF`, `SIGDEMO.ELF`, `MREACTDEMO.ELF`, `CAPDEMO.ELF`
- ELF demo: `JOBDEMO.ELF`, `NSDEMO.ELF`, `POSIXDEMO.ELF`, `MMAPDEMO.ELF`
- ELF demo: `MUSLABI.ELF`, `TLSDEMO.ELF`, `CRTDEMO.ELF`
- ELF demo: `MUSLHELLO.ELF`, `MUSLSTDIO.ELF`, `MUSLMALLOC.ELF`, `MUSLFORK.ELF`, `MUSLPIPE.ELF`, `MUSLGLOB.ELF`
- ELF demo: `MUSLDL.ELF`
- ELF demo: `CLONEDEMO.ELF`, `THREADLIFE.ELF`, `FUTEXDEMO.ELF`, `PTHREADDEMO.ELF`, `SEMDEMO.ELF`, `TLSMTDEMO.ELF`
- ELF demo: `EPOLLDEMO.ELF`, `SYSVIPC.ELF`, `SOCKDEMO.ELF`, `PTYDEMO.ELF`
- ELF demo: `ARKSHSMK.ELF`
- bundle demo: `hello.enlil/manifest.toon`, `hello.enlil/bin/hello`, `hello.enlil/lib/libdyn.so`
- ELF server: `VFSD.ELF`, `BLKD.ELF`, `NETD.ELF`
- ELF linux-compat: `LS.ELF`
- `libdyn.so`, `LD-ENLIL.SO`

Questa scelta permette un bootstrap completamente in RAM e accessi O(1) ai file iniziali, utile per configurazione iniziale, recovery, avvio dei server user-space e validazione dei demo EL0.

Da `M9-04`, i task EL0 passano anche da `vfsd` per `chdir()/getcwd()`, `mount()/umount()`,
`unshare(CLONE_NEWNS)` e `pivot_root()`: la vista del filesystem puo' quindi diventare privata
per processo senza bypassare il server VFS user-space.

---

## Build e dipendenze

### Requisiti

```bash
# macOS
brew install aarch64-elf-gcc qemu cmake

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

### Toolchain bootstrap musl

Per il profilo `M11-01` sono disponibili anche:

```bash
make musl-sysroot
make musl-smoke
make arksh-smoke
```

Questi target generano:

- `toolchain/sysroot/usr/include` con gli header bootstrap EnlilOS/musl
- `toolchain/sysroot/usr/lib/libc.a` e i runtime object `crt1.o`, `crti.o`, `crtn.o`
- wrapper `toolchain/bin/aarch64-enlilos-musl-gcc`, `ar`, `ranlib`
- smoke ELF statici musl-linked in `toolchain/smoke/*.elf`
- smoke CMake/toolchain in `toolchain/build/arksh-smoke/arkshsmoke.elf`

Per il porting `arksh` (`M8-08e`) sono disponibili anche:

```bash
make arksh-configure ARKSH_DIR=/percorso/arksh
make arksh-build ARKSH_DIR=/percorso/arksh
```

Il repo `arksh` resta esterno al tree di EnlilOS: la `v1` chiude toolchain file,
wrapper musl, compat shim di riferimento e smoke CMake. Oggi `make arksh-build`
compila il binario reale statico `toolchain/build/arksh/arksh`, e il packaging
dell'`initrd` lo include automaticamente come `/bin/arksh.real`. Da `M8-08f`
il sistema avvia una login shell `v1` tramite `/bin/arksh` (launcher/static bridge),
con fallback automatico a `/bin/nsh`, `rc` bootstrap in `/etc/arkshrc` e
`/home/user/.config/arksh/arkshrc`, e home persistente preparata su `/data/home`.
Da `M8-08 plugin`, il runtime `libdl` carica anche plugin nativi di `arksh`
dal filesystem; la reference plugin `enlil.so` viene impacchettata in
`/usr/lib/arksh/plugins/enlil.so` e validata dal selftest `arksh-plugin`.

Il profilo attuale resta volutamente `static-only` lato musl/libc, ma da `M11-02e`
la bootstrap libc espone gia' `<pthread.h>`, `<signal.h>` e `<semaphore.h>`, con:

- `pthread_create/join/detach/self/equal/kill/sigmask`
- `pthread_mutex_*` e `pthread_cond_*` sopra `futex`
- `sem_t` named/anon sopra `ksem`
- TLS statico multi-thread per `__thread`
- `errno` thread-local
- smoke `PTHREADDEMO.ELF`, `SEMDEMO.ELF` e `TLSMTDEMO.ELF`

Da `M11-05f` la libc bootstrap espone anche `<pty.h>` con:

- `posix_openpt`, `grantpt`, `unlockpt`, `ptsname`, `ptsname_r`
- `openpty`
- smoke `PTYDEMO.ELF`

Fuori scope della `v1` restano soprattutto `FUTEX_LOCK_PI`, robust futex list,
`pthread_cancel`, affinity e gli attributi scheduler completi.

Il `Makefile` supporta anche prefissi espliciti:

```bash
make CROSS=/opt/homebrew/Cellar/aarch64-elf-gcc/15.2.0/bin/aarch64-elf-
```

---

## Target QEMU

| Comando | Descrizione |
|---------|-------------|
| `make run` | seriale PL011 + `virtio-net` |
| `make run-fb` | seriale + `ramfb` + `virtio-net` |
| `make run-gpu` | seriale + `virtio-gpu` + tastiera + mouse + `virtio-net` |
| `make run-blk` | come `run-gpu` + `virtio-blk` con `disk.img` |
| `make test` | avvio del kernel selftest sotto QEMU con `virtio-net`, `virtio-gpu` e `virtio-blk`, con poweroff automatico a fine suite |
| `make debug` | QEMU in attesa di GDB sulla porta 1234 |
| `make musl-sysroot` | prepara sysroot/bootstrap libc `M11-01` |
| `make musl-smoke` | compila i demo smoke musl statici |
| `make arksh-smoke` | builda lo smoke CMake/toolchain `M8-08e` |
| `make arksh-configure ARKSH_DIR=...` | configura un checkout esterno di `arksh` |
| `make arksh-build ARKSH_DIR=...` | compila un checkout esterno di `arksh` |

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
- `pwd`, `cd /data`, `ls`, `cat /BOOT.TXT`
- `write`, `append`, `truncate`, `sync`, `fsync`
- `mkdir`, `rm`, `mv`
- `elfdemo`, `execdemo`, `dyndemo`, `forkdemo`, `sigdemo`
- `mreactdemo`, `jobdemo`, `nsdemo`, `posixdemo`
- `muslabi`, `muslglob`, `musldl`
- `arkshsmoke`
- `clonedemo`, `threadlife`, `futexdemo`
- `pthreaddemo`, `semdemo`, `tlsmtdemo`
- `ptydemo`
- `arksh`
- `net`, `socketdemo`
- `epolldemo`, `sysvipcdemo`
- `ldintdemo`
- `kbdlayout`, `loadkeys it`
- `runelf /MUSLHELLO.ELF`
- `runelf PATH`
- `nsh`
- `poweroff`, `reboot`, `halt`
- `selftest`, `selftest [nome]`

Al boot il sistema prova ad avviare `/bin/arksh` come login shell di default. Il
launcher cerca la shell reale in `/bin/arksh.real`;
se non la trova, degrada in modo pulito su `/bin/nsh`, che resta anche richiamabile
esplicitamente come recovery shell.

La tastiera usa oggi una pipeline `keycode -> keysym -> Unicode UTF-8` con due
layout integrati, `us` e `it`. `kbdlayout` mostra il layout attivo, mentre
`loadkeys us|it` lo cambia a runtime e prova a persisterlo in
`/data/etc/vconsole.conf` se il disco `ext4` e' disponibile. In fallback resta
valida la configurazione bootstrap in `/etc/vconsole.conf`.

La rete di bootstrap usa oggi `virtio-net` in modalita' raw Ethernet: il driver
kernel espone `net_send/net_recv/net_get_info`, mentre `NETD.ELF` viene lanciato
come server user-space sulla porta `net`. Il comando boot `net` mostra MAC,
stato link e contatori RX/TX. Sopra questo profilo, `M10-03` aggiunge anche una
BSD socket API `v1` per task EL0: `AF_INET`, `SOCK_STREAM` e `SOCK_DGRAM`,
solo loopback `127.0.0.1` per ora. Il comando `socketdemo` lancia
`/SOCKDEMO.ELF` e verifica echo TCP, UDP loopback e `setsockopt/getsockopt`.

Da `M11-05f` e' disponibile il PTY subsystem: `posix_openpt/grantpt/unlockpt/ptsname_r`,
apertura slave via `/dev/pts/N`, line discipline completa, `TIOCGWINSZ/TIOCSWINSZ` e
`SIGWINCH`. Il comando `ptydemo` lancia `PTYDEMO.ELF` che verifica l'intero ciclo.

`NSH` e' una shell EL0 minimale integrata nel rootfs bootstrap. Al momento espone:

- `ls`, `cat`, `echo`, `exec`, `clear`, `top`, `cd`, `pwd`, `help`, `exit`

---

## Test

Esiste una suite di self-test kernel-side che oggi copre 60 casi:

- `vfs-rootfs`, `vfs-devfs`, `ext4-core`, `vfsd-core`, `blkd-core`, `net-core`, `net-stack`
- `elf-loader`, `init-elf`, `nsh-elf`, `execve`, `exec-target`, `elf-dynamic`
- `fork-cow`, `signal-core`, `jobctl-core`, `posix-ux`, `musl-abi-core`
- `clone-thread`, `thread-lifecycle`, `futex-core`, `musl-pthread`, `musl-sem`, `tls-mt`
- `arksh-toolchain`, `arksh-login`, `vfs-namespace`, `mreact-core`, `cap-core`
- `ksem-core`, `kmon-core`, `ipc-sync`
- `kdebug-core`, `gpu-stack`, `procfs-core`
- `linux-proc-dev-etc`, `linux-fs-env`, `linux-at-paths`
- `sysv-ipc`, `mmap-file`, `tls-tp`, `crt-startup`
- `musl-hello`, `musl-stdio`, `musl-malloc`, `musl-forkexec`, `musl-pipe`, `musl-glob`, `musl-dlfcn`
- `gnu-ls`, `bash-linux-fork`, `linux-ld-shim`
- `epoll-core`, `kbd-layout`, `socket-api`
- `pty-core`, `glibc-compat`, `arksh-plugin`, `enlil-bundle`, `mmu-user-va`

La build dedicata e':

```bash
make test-build
make test
```

`make test` esegue l'autorun della suite e poi spegne automaticamente la VM
QEMU tramite `shutdown_system(SHUTDOWN_POWEROFF)`, quindi non resta aperta
in halt dopo il summary finale.

Lo stato attuale validato e':

```text
SUMMARY total=60 pass=60 fail=0
```

Nota: se il selftest si blocca prima del poweroff, conviene leggere il log seriale
completo. La suite e' pensata per isolare regressioni su mount, exec, memoria
virtuale, IPC, server user-space e stack grafico.

---

## Struttura del progetto

```text
EnlilOS/
├── boot/                  # entry point, vectors, initrd embedded
├── compat/                # shim/reference layer per porting esterni
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
│   ├── pty.c             # PTY subsystem (M11-05f)
│   ├── tty.c             # line discipline console
│   ├── term80.c          # terminale testuale 80x25
│   └── selftest.c        # suite integrata
├── tools/
│   └── mkinitrd.py       # builder host-side dell'archivio CPIO
├── toolchain/            # sysroot/bootstrap libc/wrapper musl v1
│   ├── cmake-smoke/      # smoke project CMake per M8-08e
│   └── smoke/            # demo musl statici (incluso pty_demo.c)
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
- userspace EL0 con loader dinamico, `fork()`, `mmap()` file-backed e `execve()` e' presente
- shell `NSH`, capability, `vfsd`, `blkd`, `procfs` e IPC sincrono sono presenti
- compat Linux AArch64 attiva con `bash-linux`, `epoll`, SysV IPC, PTY e shim `ld-linux`

Il path core descritto in [BACKLOG.md](BACKLOG.md) e' completato; il lavoro successivo continua nella roadmap estesa in [BACKLOG2.md](BACKLOG2.md).
