# EnlilOS — Guida per Claude

Microkernel AArch64 stile GNU Hurd. File cattura conoscenza architetturale per lavorare sul codebase senza rileggere tutto ogni sessione.

---

## Target e build

- **Architettura**: AArch64 (cortex-a72), QEMU virt, 512MB RAM
- **Toolchain**: `aarch64-elf-gcc`, `aarch64-elf-ld`
- **Output kernel**: `enlil.elf` / `enlil.bin`
- **Output selftest**: `enlil-selftest.elf` (suite separata, flag `-DENLILOS_SELFTEST=1`)
- **Comandi**:
  - `make` — build normale
  - `make test` — build selftest + lancio QEMU
  - `make run` — boot kernel normale in QEMU
  - `make musl-sysroot` — prepara sysroot/bootstrap libc `M11-01`
  - `make musl-smoke` — compila demo statici musl-linked
  - `make arksh-smoke` — smoke CMake/toolchain per `M8-08e`
  - `make arksh-configure ARKSH_DIR=...` — configura checkout esterno `arksh`
  - `make arksh-build ARKSH_DIR=...` — compila checkout esterno `arksh`
- Stato validato: `SUMMARY total=60 pass=60 fail=0`
- `make test` chiude automaticamente QEMU: dopo `SUMMARY ... PASS/FAIL` il kernel esegue `shutdown_system(SHUTDOWN_POWEROFF)` e termina la VM.
- `disk.img` lockato da QEMU: sessione appesa → successiva fallisce con "Failed to get write lock". Usare `ps ... | rg qemu-system-aarch64` poi `kill <pid>`.

---

## Struttura directory

```
boot/          — boot.S (entry AArch64), vectors.S (tabella eccezioni)
kernel/        — tutti i moduli kernel (.c)
kernel/sched_switch.S — context switch assembly
include/       — header pubblici
drivers/       — uart, keyboard, mouse, blk, framebuffer, ane/, gpu/
user/          — programmi EL0 (demo, nsh, vfsd, blkd, ecc.)
tools/         — gen_ksyms.py e altri script di build
toolchain/     — sysroot/bootstrap libc/wrapper `aarch64-enlilos-musl-*`
compat/        — shim/reference layer per porting esterni (`arksh`)
```

---

## Scheduler (FPP — Fixed Priority Preemptive)

**File**: [kernel/sched.c](kernel/sched.c), [kernel/sched_switch.S](kernel/sched_switch.S), [include/sched.h](include/sched.h)

### Priorità (0=massima, 255=minima)
```c
PRIO_KERNEL = 0    // massima, usata da selftest e task RT
PRIO_HIGH   = 32
PRIO_NORMAL = 128
PRIO_LOW    = 200
PRIO_IDLE   = 255  // task idle, sempre READY
```

- `SCHED_MAX_TASKS = 96`
- Pool task non ricicla ancora TCB zombie; con profilo attuale (`M10-03` + `M11-03` inclusi) suite completa sale a `47/47`, supera vecchio limite cumulativo 64 task. Valore 96 intenzionale, allineato crescita bring-up userspace.

### TCB (`sched_tcb_t`) — esattamente 64 byte, layout fisso
```
offset  0: sp            — SP salvato (CRITICAMENTE a offset 0, dipende sched_switch.S)
offset  8: pid
offset 12: priority      — priorità base (0=max)
offset 13: state         — TCB_STATE_RUNNING/READY/BLOCKED/ZOMBIE
offset 14: flags         — TCB_FLAG_KERNEL/IDLE/RT/USER
offset 15: ticks_left
...
offset 56: next          — link intrusive nella run queue
```
**NON modificare layout TCB** — `sched_switch.S` dipende da `sp` a offset 0.

### Run queue
- `rq_head[256]` / `rq_tail[256]`: FIFO singly-linked per priorità
- `ready_bitmap[4]`: 256 bit, `bitmap_find_first()` → O(1) via CTZ
- `rq_push(t)`: aggiunge a coda bucket `eff_prio_of(t)`. **Pericolo doppio-push**: corrupe catena.
- `rq_pop(p)`: estrae da testa, setta `t->next = NULL`
- `rq_remove(t, p)`: scansione lineare, silent fail se non trovato

### Priority Inheritance (PI)
- `donated_priority[slot]`: per-task, monotonicamente decrescente (0xFF = nessuna donazione)
- `eff_prio_of(t) = min(t->priority, donated_priority[slot])`
- `sched_task_donate_priority(t, prio)`: aggiorna donated_priority. Se t READY → `rq_remove(t, old_eff)` + `rq_push(t)`. Se t BLOCKED o RUNNING → non tocca run queue.
- `sched_task_clear_donation(t)`: resetta donated_priority a 0xFF.

### PERICOLO: direct priority mutation
**Non fare MAI** `task->priority = X` mentre task è READY nella run queue.
Task nel bucket `eff_prio_of()` calcolato PRIMA della mutazione. Cambiando `priority` direttamente: `eff_prio_of` cambia ma task ancora nel vecchio bucket → `rq_remove(t, nuovo_eff)` fallisce silenziosamente → `rq_push` aggiunge nel nuovo bucket senza rimuovere dal vecchio → task in **due bucket contemporaneamente** → corruzione run queue → crash.
Usare sempre `sched_task_donate_priority()`.

### Context switch (`sched_context_switch`)
Salva callee-saved di prev sullo stack, ripristina di next:
```
[sp+ 0] x19, x20
[sp+16] x21, x22
[sp+32] x23, x24
[sp+48] x25, x26
[sp+64] x27, x28
[sp+80] x29 (fp), x30 (lr)
```
`ret` salta a x30. Per task al primo avvio: x30 = `task_entry_trampoline`.

### Invariante IRQ in `schedule_locked`
IRQ rimangono **disabilitati** durante `sched_context_switch`. NON usare `irq_restore(flags)` prima del context switch: aprirebbe finestra dove timer IRQ chiamerebbe `schedule()` rientrante, corrompendo `next->sp` → crash (PC=0, x30=0). Dopo context switch si usa `msr daifclr, #2` (non `irq_restore`) per riabilitare incondizionatamente IRQ — task ripreso potrebbe avere flags con I=1 (era bloccato in syscall handler dove HW imposta DAIF.I=1 all'ingresso eccezione).

### sched_block vs sched_yield
- `sched_block()`: state=BLOCKED, task non re-inserito in coda. Richiede `sched_unblock()` esplicito.
- `sched_yield()`: task resta READY, re-inserito in coda dopo reschedule.
- `sched_unblock(t)`: agisce solo se `t->state == TCB_STATE_BLOCKED`.

### Preemption hardware
Dopo ogni IRQ in `vectors.S`: se `need_resched=1` → `schedule()`. Timer chiama `sched_tick()` ogni 1ms, decrementa quantum e setta `need_resched`.

---

## Syscall ABI

**File**: [include/syscall.h](include/syscall.h), [kernel/syscall.c](kernel/syscall.c)

- Numero syscall in **x8**, argomenti in **x0–x5**, valore ritorno in **x0**
- Valori negativi = `-errno`
- Tabella `syscall_table[256]` indicizzata direttamente (O(1))
- `ERR(e)` = `(uint64_t)(-(int64_t)(e))` — macro per costruire codici d'errore

### Assegnazioni stabili
```
1–20    base POSIX (write/read/exit/open/close/fstat/mmap/munmap/brk/execve/
         fork/waitpid/clock_gettime/getdents/task_snapshot/spawn/
         sigaction/sigprocmask/sigreturn/yield)
39–55   ABI minima musl v1 (getpid/getppid/gettimeofday/nanosleep/uids/
         lseek/readv/writev/fcntl/openat/fstatat/ioctl/uname)
56–59   baseline threading (clone/gettid/set_tid_address/exit_group)
65–66   futex + tgkill
60–64   capability (cap_alloc/cap_send/cap_revoke/cap_derive/cap_query)
80–84   mreact (subscribe/wait/cancel/subscribe_all/subscribe_any)
85–94   ksem (create/open/close/unlink/post/wait/timedwait/trywait/getvalue/anon)
95–99   kmon (create/destroy/enter/exit/wait)
100–119 ANE
110–111 kmon_signal / kmon_broadcast
120–139 GPU
28–29   chdir/getcwd
34–38   pipe/dup/dup2/tcgetattr/tcsetattr
41      isatty
134     kill
140–142 IPC (port_lookup/ipc_wait/ipc_reply)
150–155 VFS boot (vfs_boot_open/read/write/readdir/stat/close)
156–159 BLK boot (blk_boot_read/write/flush/sectors) — solo owner porta "block"
160–161 VFS boot extra (taskinfo/lseek)
```

### Wrapper EL0 (`include/user_svc.h`)
Per programmi user-space usare sempre `user_svc0..6()` e `user_svc_exit()`. Non scrivere inline assembly SVC nei file `.c` dei demo.

---

## Semafori kernel (`ksem`)

**File**: [kernel/ksem.c](kernel/ksem.c), [include/ksem.h](include/ksem.h)

- Pool statico: `ksem_pool[64]`, `ksem_waiters[64]`, `ksem_refs[512]`
- Wait con `sched_yield()` (busy-wait cooperativo), non `sched_block()`
- PI: `ksem_update_pi_locked()` → `sched_task_donate_priority(owner_hint, best_waiter_prio)`
- Flag `KSEM_RT`: attiva PI e `owner_hint` tracking
- `ksem_task_cleanup(task)`: chiamato da `sched_task_exit_with_code()` a exit task

### Pattern selftest ksem
- **holder** deve avere priorità **PRIO_HIGH** (non PRIO_NORMAL). Con PRIO_NORMAL verrebbe starved dall'hog task (stesso livello, inserito prima in FIFO).

---

## Monitor kernel (`kmon`)

**File**: [kernel/kmon.c](kernel/kmon.c), [include/kmon.h](include/kmon.h)

- Semantica Mesa-style (condition variable + mutex integrato)
- `kmon_wait_current()`: usa `sched_yield()`, non `sched_block()`
- `kmon_refresh_owner_locked()`: usa `sched_task_donate_priority()` — **mai** `task->priority =`
- Priority ceiling per monitor RT: donazione automatica a `kmon_enter`
- `kmon_task_cleanup(task)`: chiamato a exit

---

## Capability System (`cap`)

**File**: [kernel/cap.c](kernel/cap.c), [include/cap.h](include/cap.h)

- `cap_pool[256]`: pool globale capability attive/libere
- `cap_table[SCHED_MAX_TASKS][64]`: per-task, array token posseduti
- Token 64-bit generato con `CNTPCT_EL0 ^ pid ^ cap_salt` (non indovinabile)
- `cap_alloc_kernel(pid, type, rights, object)`: API interna kernel
- Syscall: `cap_alloc(60)`, `cap_send(61)`, `cap_revoke(62)`, `cap_derive(63)`, `cap_query(64)`
- Solo `owner_pid` può revocare; `cap_derive` non può aggiungere diritti rispetto al padre

---

## VFS server user-space (`vfsd`)

**File**: [user/vfsd.c](user/vfsd.c)

- Task EL0 pid=4, porta IPC "vfs"
- Usa syscall `vfs_boot_*` (150–155) per accedere VFS kernel-side
- Selftest `vfsd-core` verifica risposta corretta a open/read/stat
- Bootstrap: spawned da `kernel/main.c` come primo task user-space
- Da `M11-02a`, `vfsd` ragiona per `tgid` non per singolo sender TID: tutti thread stesso processo condividono `cwd` e mount namespace.
- Nota critica M11-01a: `lseek()` su fd remoti richiede sia `VFSD_REQ_LSEEK` lato IPC sia aggiornamento `file.pos` shadow nel kernel dopo `read/write`. Aggiornando solo remote handle e non shadow, `SEEK_CUR` parte da posizione stantia e demo ELF falliscono in modo intermittente.

## ABI minima musl (`M11-01a`)

**File**: [kernel/syscall.c](kernel/syscall.c), [include/syscall.h](include/syscall.h),
[user/musl_abi_demo.c](user/musl_abi_demo.c)

- Implementato pacchetto v1: `getpid/getppid/gettimeofday/nanosleep`, `getuid/getgid/geteuid/getegid`, `lseek`, `readv`, `writev`, `fcntl`, `openat`, `fstatat/newfstatat`, `ioctl`, `uname`
- `O_CLOEXEC` / `FD_CLOEXEC` reali: `open/openat` li memorizzano su fd entry e `execve()` chiude fd marcati solo sul path di successo
- `fcntl` v1 supporta `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`, `F_DUPFD`, `F_DUPFD_CLOEXEC`
- `openat()` v1 supporta `AT_FDCWD` e path assoluti; dirfd relativi veri rinviati
- `fstatat()` v1 supporta `AT_FDCWD` e `AT_EMPTY_PATH`
- `ioctl()` v1 supporta `TCGETS`, `TCSETS`, `TIOCGWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`, `FIONBIO`; fallback `-ENOTTY`
- Demo runtime: `/MUSLABI.ELF`, comando boot `muslabi`, selftest `musl-abi-core`
- `MUSLABI.ELF` verifica anche `auxv` minima per musl: `AT_RANDOM`, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`
- `AT_RANDOM` costruita nel loader ELF direttamente sullo stack user-space: 16 byte pseudocasuali scritti in memoria utente, referenziati via puntatore nell'`auxv`
- `M11-01b` chiusa in v1: `crt1/crti/crtn`, `environ`, `__enlilos_auxv`, `preinit/init/fini arrays` e startup C statico validati da `CRTDEMO.ELF`
- Gotcha critico risolto: restore di `TPIDR_EL0` non può stare "dopo" `sched_context_switch()` assumendo che quel codice giri nel task `next`; quando funzione riprende, gira nel task appena tornato in esecuzione. Fix corretta:
  - salvare sempre TP task uscente prima dello switch
  - ripristinare `TPIDR_EL0` dal `current_task` reale quando task riprende
  - impostare `TPIDR_EL0` anche nel primo ingresso EL0 da `sched_task_bootstrap()`
- Altro gotcha: `PT_TLS` con `p_memsz == 0` va ignorato, linker può emetterlo anche per ELF senza TLS reale
- Layout TLS statico v1 funzionante con toolchain attuale: `[TCB stub 16B][tdata][tbss zeroed/aligned]`, con `TPIDR_EL0` puntato al TCB

## Toolchain musl bootstrap (`M11-01c`)

**File**: [toolchain/enlilos-musl/include](toolchain/enlilos-musl/include),
[toolchain/enlilos-musl/src](toolchain/enlilos-musl/src),
[toolchain/bin/aarch64-enlilos-musl-gcc](toolchain/bin/aarch64-enlilos-musl-gcc),
[toolchain/bin/enlilos-toolchain-common.sh](toolchain/bin/enlilos-toolchain-common.sh),
[tools/enlilos-aarch64.cmake](tools/enlilos-aarch64.cmake)

- `M11-01c` non è porting upstream completo di musl: bootstrap statico sufficiente per compilare ed eseguire programmi C semplici su EnlilOS.
- Sysroot vive in `toolchain/sysroot/usr/{include,lib}`, popolato da:
  - header bootstrap in `toolchain/enlilos-musl/include`
  - `libc.a` minimale costruita da `toolchain/enlilos-musl/src`
  - `crt1.o`, `crti.o`, `crtn.o` copiati dalla build user-space esistente
- Wrapper `aarch64-enlilos-musl-gcc` deve passare sia `--sysroot` sia `-isystem "$SYSROOT/usr/include"`; con solo `--sysroot` alcuni include non risolti come previsto nella toolchain attuale.
- Bootstrap libc non deve includere direttamente `include/syscall.h` del kernel: trascina tipi kernel-side che collidono con `stdint.h/stddef.h`. Usare header privato `toolchain/enlilos-musl/src/enlil_syscalls.h`.
- Nella rule build bootstrap libc, ordine include corretto: `-Itoolchain/enlilos-musl/include -Iinclude`. Invertirlo rompe build per collisioni typedef e macro.
- `environ` definita dal runtime `crt1`; nella libc bootstrap va solo dichiarata `extern`, altrimenti link fallisce per simbolo duplicato.
- Smoke test runtime: `MUSLHELLO.ELF`, `MUSLSTDIO.ELF`, `MUSLMALLOC.ELF`, `MUSLFORK.ELF`, `MUSLPIPE.ELF`, `MUSLGLOB.ELF`.
- `tools/enlilos-aarch64.cmake` usato anche per progetti CMake esterni: definisce `ENLILOS=1`, usa wrapper `aarch64-enlilos-musl-*` e sysroot bootstrap in-tree.
- Gotcha emerso con `M8-08e`: lanciando CMake da dentro `Makefile` senza pulire ambiente, flag kernel-side (`-T linker.ld -nostdlib`) trapelano nel progetto esterno e link fallisce. Target `arksh-configure` e `arksh-smoke` devono eseguire `env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS`.
- `M8-08e` non vendorizza `arksh`: sorgente resta esterno, passato via `ARKSH_DIR=/percorso/arksh`. In-tree restano compat shim di riferimento `compat/arksh/enlilos.c` e smoke CMake `toolchain/cmake-smoke/`.
- Validazione `M8-08e`:
  - target host-side `make arksh-smoke`
  - ELF runtime `/ARKSHSMK.ELF`
  - comando boot `arkshsmoke`
  - selftest `arksh-toolchain`
- Bug reale da bring-up: `fd_pipe_read()` non deve tentare di riempire tutto il buffer richiesto su una pipe. Deve tornare appena ha letto byte disponibili, altrimenti `musl-pipe` resta bloccato dopo aver consumato il payload presente.
- `dirent.h` / `dirent.c` bootstrap libc volutamente minimali, leggono directory entry per entry via `SYS_GETDENTS`; contratto pratico oggi allineato al payload kernel `name[32] + mode`.
- `glob()` / `fnmatch()` già disponibili in v1 per bootstrap shell-side: `fnmatch()` gestisce `* ? []` con `FNM_PATHNAME`, `FNM_NOESCAPE`, `FNM_PERIOD`; `glob()` espande wildcard sopra `opendir/readdir` senza nuove syscall.
- Gotcha fondamentale `glob()`: `malloc()` bootstrap fa `mmap()` per ogni allocazione. Modello "una stringa, una allocazione" esplode presto in `GLOB_APPEND`. Soluzione valida: store compatto unico che contiene sia `gl_pathv` sia pool delle stringhe matchate.

## Thread-group baseline (`M11-02a`)

**File**: [kernel/sched.c](kernel/sched.c), [include/sched.h](include/sched.h),
[kernel/syscall.c](kernel/syscall.c), [kernel/signal.c](kernel/signal.c),
[user/vfsd.c](user/vfsd.c), [user/clone_demo.c](user/clone_demo.c)

- `M11-02a` non introduce ancora `pthread`, ma chiude base kernel: `getpid()` ora ritorna `tgid`, mentre `gettid()` espone TID reale.
- Stato condiviso processo non è oggetto heap dinamico: vive in `proc_ctx[SCHED_MAX_TASKS]` dentro `kernel/sched.c`, referenziato da `proc_slot`. Tutto ciò che deve essere condiviso tra thread va agganciato a `proc_slot`, non a `pid`.
- Dopo `M11-02a`, `fd_tables`, `task_brk`, `vfs_srv_tables` e ownership VMM/VFS keyed per `proc_slot`, non per TID.
- Signal dispositions (`sigaction`) condivise per processo slot; campi `pending`, `blocked` e stato stop/resto restano per-thread.
- Dopo `M11-02b`, `signal_send_pid()` è process-directed per `tgid`, mentre `signal_send_tgkill()` fa delivery thread-directed sul `tid` esatto.
- `clone()` v1 supporta solo subset thread-oriented: `CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD` più `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`.
- `fork()` ed `execve()` ritornano ancora `-EBUSY` in processi multi-thread. Limite intenzionale finché non esiste path pthread/futex completo e meglio validato oltre `M11-02b`.
- Gotcha critico: non liberare subito metadati processo quando ultimo thread esce. Azzerare `proc_slot` troppo presto rompe `waitpid()`, `SIGCHLD`, job control e demo come `nsdemo`/`musl-forkexec`. Parte da rilasciare subito è `mm`; metadati zombie devono restare vivi fino al reap.
- `CLONE_CHILD_CLEARTID` oggi chiude ciclo completo solo insieme al futex core di `M11-02c`: clear in memoria utente + wake del waiter.
- Validazione attuale:
  - selftest `clone-thread`
  - demo `/CLONEDEMO.ELF`
  - suite completa `42/42`

---

## Thread lifecycle (`M11-02b`)

**File**: [kernel/sched.c](kernel/sched.c), [include/sched.h](include/sched.h),
[kernel/syscall.c](kernel/syscall.c), [kernel/signal.c](kernel/signal.c),
[user/thread_life_demo.c](user/thread_life_demo.c)

- `set_tid_address()` salva `clear_child_tid` per-thread e ritorna TID corrente.
- `exit_group()` non è sinonimo di `exit()`: marca `proc_slot` come group-exiting, termina altri thread del gruppo, fa convergere codice finale processo sul leader waitable.
- Processo multi-thread diventa reapable da `waitpid()` **solo** quando esce ultimo thread. Prima di quel momento, leader zombie non deve essere considerato "process exit completed".
- `signal_terminate_current()` deve passare da `sched_task_exit_with_code()`, non zombificare task da sola. Altrimenti `clear_child_tid`, cleanup `ksem/kmon/mreact` e accounting processo si rompono.
- `tgkill(tgid, tid, sig)` è path corretto per segnali thread-directed. Va usato anche per eccezioni sincrone del task EL0 corrente.
- `clear_child_tid` oggi fa solo clear affidabile in memoria utente. Wake dei waiter chiuso da `M11-02c`; non tentare comunque di modellare `pthread_join()` sopra `waitpid()`, path corretto resta `futex`.
- Validazione attuale:
  - selftest `thread-lifecycle`
  - demo `/THREADLIFE.ELF`
  - suite completa `42/42`

---

## Futex core (`M11-02c`)

**File**: [kernel/futex.c](kernel/futex.c), [include/futex.h](include/futex.h),
[kernel/syscall.c](kernel/syscall.c), [kernel/sched.c](kernel/sched.c),
[user/futex_demo.c](user/futex_demo.c)

- `futex` v1 supporta `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`. `FUTEX_PRIVATE_FLAG` accettato come hint.
- Chiave non è solo `uaddr`: è `(proc_slot, uaddr)`. Intenzionale: v1 copre bene thread stesso processo/address-space, non promette ancora caso cross-process shared mapping.
- Waiter vivono in array statico per-task, senza allocazioni dinamiche nel path caldo. Se thread esce mentre bloccato su futex, `futex_task_cleanup()` lo rimuove dalla bucket list.
- Wake su `clear_child_tid` avviene nel path di uscita in `sched_task_finish_exit()`. Chiude base necessaria per costruire `pthread_join()` sopra futex.
- Timeout, `WAIT_BITSET`, robust futex list e `FUTEX_LOCK_PI` restano fuori scope di questa fase.
- Validazione attuale:
  - selftest `futex-core`
  - demo `/FUTEXDEMO.ELF`
  - suite completa `42/42`

---

## musl `pthread` + `sem_t` bootstrap (`M11-02d`)

**File**: [toolchain/enlilos-musl/src/pthread.c](toolchain/enlilos-musl/src/pthread.c),
[toolchain/enlilos-musl/src/semaphore.c](toolchain/enlilos-musl/src/semaphore.c),
[toolchain/enlilos-musl/include/pthread.h](toolchain/enlilos-musl/include/pthread.h),
[toolchain/enlilos-musl/include/semaphore.h](toolchain/enlilos-musl/include/semaphore.h),
[toolchain/enlilos-musl/include/signal.h](toolchain/enlilos-musl/include/signal.h),
[user/crt1.c](user/crt1.c), [kernel/ksem.c](kernel/ksem.c)

- `crt1` chiama debolmente `__enlilos_thread_runtime_init()` prima dei costruttori: serve per bootstrapare thread principale senza imporre runtime pthread ai binari che non lo usano.
- Runtime `pthread` v1 usa primo word dello stub puntato da `TPIDR_EL0` per stashare puntatore al `pthread_t` corrente. Thread figli lanciati con `CLONE_SETTLS` e stub minimo dedicato.
- `pthread_join()` non usa `waitpid()`: aspetta `clear_child_tid` tramite `futex`.
- `pthread_mutex_*` e `pthread_cond_*` chiusi sopra `futex`; `sem_t` named/anon chiuso sopra `ksem`.
- Handle `ksem` per task user-space devono essere keyed per `tgid`, non per `tid`. Keyed per thread: semafori anonimi/named creati da un thread non visibili ai sibling thread stesso processo.
- `sem_timedwait()` usa `abstime` assoluto. Oggi `gettimeofday()` vicino al boot time, può partire da `0s`: test che costruisce `past = now - 1s` può generare correttamente `EINVAL` se va sotto zero; per ottenere `ETIMEDOUT` il deadline scaduto deve restare valido/non negativo.
- Limite storico v1 su `__thread` chiuso da `M11-02e`: thread figli ora allocano blocco TLS completo dal template `PT_TLS`, quindi `errno` e variabili TLS statiche non condividono più stato tra thread.
- Validazione attuale:
  - selftest `musl-pthread`
  - selftest `musl-sem`
  - demo `/PTHREADDEMO.ELF`
  - demo `/SEMDEMO.ELF`
  - suite completa `42/42`

---

## TLS multi-thread hardening (`M11-02e`)

**File**: [toolchain/enlilos-musl/src/pthread.c](toolchain/enlilos-musl/src/pthread.c),
[toolchain/enlilos-musl/src/errno.c](toolchain/enlilos-musl/src/errno.c),
[toolchain/smoke/musl_tls_mt.c](toolchain/smoke/musl_tls_mt.c),
[kernel/selftest.c](kernel/selftest.c)

- Runtime `pthread` non deve clonare solo stub da 16 byte: per thread figli serve blocco completo compatibile col layout loader-side `[TCB stub 16B][tdata][tbss]`.
- Sorgente corretta del template non è TLS del thread corrente, ma segmento `PT_TLS` dell'ELF. Runtime lo recupera via `auxv` (`AT_PHDR/AT_PHNUM`) e copia `p_filesz` bytes dal template del binario, zeroando tail `tbss`.
- `errno` nel bootstrap musl può e deve essere `__thread` una volta chiuso TLS multi-thread; prima di questo, tutti i thread condividevano accidentalmente lo stesso `errno`.
- Smoke `TLSMTDEMO.ELF` verifica quattro cose insieme:
  - init template TLS per ogni thread
  - isolamento `__thread` cross-thread
  - `errno` per-thread
  - join corretto dopo lavoro concorrente
- Validazione attuale:
  - selftest `tls-mt`
  - demo `/TLSMTDEMO.ELF`
  - suite completa `42/42`

---

## Block server user-space (`blkd`)

**File**: [user/blkd.c](user/blkd.c), [include/blk_ipc.h](include/blk_ipc.h)

- Task EL0 pid=5, porta IPC "block", priorità PRIO_LOW
- Usa syscall `blk_boot_*` (156–159) per accedere driver virtio-blk kernel-side
- Guard `blk_srv_owner_ok()`: solo processo owner porta "block" può chiamarle
- IPC payload compatto: `blkd_request_t` (16 B) + `blkd_response_t` (16 B) → dati I/O non transitano nell'IPC, blkd usa buffer statico interno (`blkd_io_buf`)
- Selftest `blkd-core` verifica: porta registrata, owner user-space, `blk_is_ready()`, capacità
- Bootstrap: `boot_launch_blkd()` in `kernel/main.c`, chiamato dopo `boot_launch_vfsd()`

### Pattern blk_boot_* vs vfs_boot_*
Identico al pattern vfsd: syscall `blk_boot_*` accessibili **solo** al task che possiede porta "block" (verificato con `mk_port_lookup("block")->owner_tid`). Evita escalation privilegi: processo EL0 arbitrario non può leggere disco raw.

---

## mreact (Reactive Memory Subscriptions)

**File**: [kernel/mreact.c](kernel/mreact.c), syscall 80–84

- Permette a task di sottoscrivere notifiche su range di indirizzi fisici
- `mreact_subscribe(addr, len, flags)` → handle
- `mreact_wait(handle, timeout)` → blocca fino a modifica nell'area
- Usato internamente dal VFS per notifiche di I/O

---

## Exception vector e IRQ

**File**: [boot/vectors.S](boot/vectors.S)

- Ogni entry vector table fa 5 istruzioni (sub/stp/mov/mov/b)
- `__exc_common`: salva frame 288 byte, chiama `exception_handler(src, typ, frame)`
- Dopo `exception_handler`: controlla `need_resched`, eventualmente chiama `schedule()`
- Frame eccezione layout:
  ```
  0..240   x0..x30
  248      sp_el0
  256      elr_el1
  264      spsr_el1
  272      esr_el1
  280      far_el1
  ```
- `ERET` al termine ripristina `SPSR_EL1` che contiene DAIF del contesto interrotto (incluso I=0 → IRQ abilitati per task in esecuzione normale)

---

## Memoria fisica e virtuale

- **PMM**: buddy allocator + slab. `phys_alloc_page()` → pagina 4KB
- **MMU**: 3 livelli, pagine 4KB
  - L1[0]: MMIO 0x00000000–0x3FFFFFFF (Device-nGnRnE)
  - L1[1]: RAM  0x40000000–0x7FFFFFFF (Normal WB)
- **Kernel heap**: named typed caches (`task_cache` 64B, `port_cache` 64B, `ipc_cache` 512B)
- Stack task kernel: 16 KiB (`TASK_STACK_ORDER = 2`), allocata con `phys_alloc_pages()`
- `TASK_STACK_SIZE = 16384`, `SCHED_MAX_TASKS = 96`

---

## ELF loader e user-space

- **Static loader**: `elf64_spawn_path()`, `elf64_load_from_path()`
- **Dynamic loader**: `ld_enlil.so` in `user/ld_enlil.c` (custom, non musl)
- Binari ELF user-space embedded nell'immagine kernel tramite `*.embed.o`, accessibili via initrd CPIO
- ELF user entrano a EL0 via `sched_enter_user()` (assembly in sched_switch.S)
- `sched_task_bootstrap()` decide kernel vs user vs resume-from-frame

---

## IPC sincrono

**File**: [kernel/microkernel.c](kernel/microkernel.c)

- Rendez-vous sincrono: client blocca fino alla risposta server
- Priority donation: server eredita priorità del client più urgente
- Budget per-porta: `mk_port_set_budget()` per latency tracking
- Syscall: `port_lookup(140)`, `ipc_wait(141)`, `ipc_reply(142)`

---

## Self-test suite

**File**: [kernel/selftest.c](kernel/selftest.c)

58 test case in ordine:
1. `vfs-rootfs` — mount initrd, readdir, stat file
2. `vfs-devfs` — devfs /dev/stdin /dev/stdout
3. `ext4-core` — mount rw ext4, journal replay
4. `vfsd-core` — VFS server user-space
5. `blkd-core` — Block server user-space (porta "block", blk_is_ready, capacità)
6. `net-core` — driver virtio-net + netd alive
7. `net-stack` — TCP/IP stack user-space (GARP inviato, tx_packets > 0)
8. `elf-loader` — carica ELF statico in EL0
9. `init-elf` — parse INIT.ELF
10. `nsh-elf` — parse NSH.ELF
11. `execve` — execve() completo
12. `exec-target` — target execve con EXEC2.ELF
13. `elf-dynamic` — carica ELF PIE con ld_enlil.so
14. `fork-cow` — fork() + COW + output su file ext4
15. `signal-core` — sigaction/sigreturn/kill
16. `jobctl-core` — process groups/sessioni/TTY job control
17. `posix-ux` — pipe/dup/cwd/env/termios
18. `musl-abi-core` — ABI minima musl + auxv
19. `clone-thread` — `clone()` thread-oriented + `tgid/gettid`
20. `thread-lifecycle` — `set_tid_address/tgkill/exit_group/clear_child_tid`
21. `futex-core` — `FUTEX_WAIT/WAKE/REQUEUE/CMP_REQUEUE` + join base
22. `musl-pthread` — bootstrap `pthread` sopra `clone/futex`
23. `musl-sem` — `sem_t` bootstrap sopra `ksem`
24. `tls-mt` — TLS statico multi-thread per `__thread`
25. `arksh-toolchain` — smoke toolchain/CMake per port hosted
26. `arksh-login` — login shell bridge e layout `/home`
27. `vfs-namespace` — mount namespace + pivot_root
28. `mreact-core` — reactive subscriptions
29. `cap-core` — capability alloc/query/derive/revoke
30. `ksem-core` — semafori RT + priority inheritance
31. `kmon-core` — monitor + PI ceiling
32. `ipc-sync` — IPC sincrono + priority donation
33. `kdebug-core` — crash reporter + ksymtab
34. `gpu-stack` — VirtIO-GPU scanout + renderer 2D
35. `procfs-core` — mount /proc + snapshot base
36. `linux-proc-dev-etc` — /proc/<pid>/{status,cmdline,...}, /dev/fd, /etc stub
37. `linux-fs-env` — /proc/sys, /etc/locale.conf, /etc/ld.so.cache, /etc/localtime
38. `linux-at-paths` — AT_FDCWD, openat, fstatat, symlinkat, readlinkat, utimensat
39. `sysv-ipc` — shmget/shmat/semget/semop
40. `mmap-file` — mmap/msync/munmap file-backed
41. `tls-tp` — preservazione `TPIDR_EL0`
42. `crt-startup` — ctor/env/auxv/dtor
43. `musl-hello` — smoke `write/open/close`
44. `musl-stdio` — smoke `printf/snprintf`
45. `musl-malloc` — smoke `malloc/calloc/realloc`
46. `musl-forkexec` — smoke `fork/execve/waitpid`
47. `musl-pipe` — smoke `pipe/dup2/termios`
48. `musl-glob` — smoke `glob()/fnmatch()` sopra VFS
49. `musl-dlfcn` — `dlopen/dlsym/dlclose/dlerror`
50. `gnu-ls` — binario GNU ls statico tramite linux-compat
51. `bash-linux-fork` — bash-linux fork+execve child
52. `linux-ld-shim` — PT_INTERP /lib/ld-linux-aarch64.so.1 alias
53. `epoll-core` — epoll_create1/ctl/pwait
54. `kbd-layout` — layout tastiera `us/it` e persistenza
55. `socket-api` — BSD socket API `AF_INET` loopback TCP/UDP
56. `pty-core` — PTY master/slave lifecycle, termios, TIOCGWINSZ/TIOCSWINSZ
57. `glibc-compat` — glibc shim dlopen: gnu_get_libc_version, __libc_start_main, __stack_chk_guard
58. `mmu-user-va` — mmu_read_user/write_user/remap_user_region kernel-side

### Helper macro
```c
ST_CHECK(case_name, cond, detail)  // ritorna -1 se !cond
```

### Pattern test con task ausiliari
Per task ausiliari (holder/hog/waiter):
- **holder** va a `PRIO_HIGH` se deve acquisire risorsa prima di essere starved
- Non mutare mai `task->priority` direttamente — usare `sched_task_donate_priority()`
- Per aspettare task: loop su `timer_now_ms()` con `sched_yield()` e deadline

---

## Milestone completate (stato 2026-04-27)

Tutto backlog 1 (M1–M7), backlog 2 fino a `M9-04`, `M10-01/02/03`, `M11-01`, `M11-02a/b/c/d/e`, `M11-03`, `M11-05a/b/c/d/e/f/g`, `M11-08` e `M8-08d/e/f/g` completi in v1, piu' `M8-08 plugin` chiusa in v1.
Aggiunti fuori-milestone: fix kbd ring buffer OOB (62° char freeze), `prlimit64` nativo (SYS_PRLIMIT64=212), shutdown/poweroff completo (PSCI + SYS_REBOOT=213).
Run di riferimento:

```text
SUMMARY total=60 pass=60 fail=0
```

**Prossime priorità** (ordine consigliato):
1. **M8-08h** — i18n / localizzazione stringhe
2. **M12-01** — Wayland server minimale (Weston-lite sopra VirtIO-GPU)
3. **M11-07** — Container primitives: namespace net, pid, uts; `pivot_root` hardening; `cgroups` v1 minimali
4. **M13-02** — SMP bootstrap
5. **M13-03** — scheduler multicore
### Stato operativo M11-05e / Linux filesystem environment

- `M11-05e` e' chiusa in `v1`.
- **`/proc/sys` subtree**: `/proc/sys` (dir), `/proc/sys/kernel` (dir), `/proc/sys/vm` (dir), `/proc/sys/kernel/pid_max` (`32768\n`), `/proc/sys/vm/overcommit_memory` (`0\n`).
- **`/proc/<pid>/maps`**: stub presente (file esiste, contenuto vuoto; page-table walk completo rinviato).
- **`/etc/locale.conf`**: `LANG=en_US.UTF-8` in initrd.
- **`/etc/ld.so.cache`**: stub binario minimo in initrd (magic glibc-ld.so.cache1.1, 0 entries).
- **`/etc/localtime`**: TZif2 UTC copiato da host macOS (`/usr/share/zoneinfo/UTC`, 114 B).
- Selftest: `linux-fs-env`. Suite: `56/56`.

### Stato operativo M11-05f / PTY (Pseudo-Terminal)

**File**: [include/pty.h](include/pty.h), [kernel/pty.c](kernel/pty.c),
[toolchain/enlilos-musl/src/pty.c](toolchain/enlilos-musl/src/pty.c),
[toolchain/enlilos-musl/include/pty.h](toolchain/enlilos-musl/include/pty.h),
[toolchain/smoke/pty_demo.c](toolchain/smoke/pty_demo.c)

- `M11-05f` chiusa in `v1`.
- PTY pool statico 8 slot (`PTY_MAX=8`), ring buffer m2s/s2m da 4096 B, edit buffer canonico 256 B.
- devfs integration: `DEV_NODE_PTMX=11`, `DEV_NODE_PTS_BASE=12..19`, `DEV_NODE_PTS_DIR=20`.
- Apertura master: `open("/dev/ptmx")` → `pty_alloc()`, `locked=1`. Slave apribile solo dopo `unlockpt` (TIOCSPTLCK ioctl con value 0).
- Apertura slave: `open("/dev/pts/N")` → `pty_get(N)`, check `!locked`, `pty_open_slave()`.
- Line discipline (`pty_ld_byte`): ICRNL, ISIG (^C→SIGINT, ^Z→SIGTSTP), ICANON (edit buffer, commit su '\n', VERASE, VKILL, VEOF), ECHO/ECHOE. In raw mode: bypass LD, byte diretti in m2s.
- Slave write: OPOST+ONLCR espande '\n' → '\r\n' verso s2m.
- `pty_slave_read` aggiorna `slave_pgid` da `current_task` a ogni call (come `tty_adopt_current_session`).
- `TIOCSWINSZ` su master: aggiorna `winsize` e invia `SIGWINCH` a `slave_pgid`.
- `SIGWINCH=28` default action = **ignore** (POSIX). Critico: mancanza nella lista `signal_default_ignore` causava terminazione del processo demo invece del semplice notify.
- musl bootstrap: `posix_openpt` (via `openat("/dev/ptmx")`), `grantpt` (no-op), `unlockpt` (TIOCSPTLCK), `ptsname_r` (TIOCGPTN + snprintf), `ptsname` (buffer statico), `openpty`.
- **Bug critico risolto: double PTY alloc via vfsd+shadow**. Quando vfsd è disponibile, `fd_open_path_current` usa `vfsd_proxy_open` (alloca PTY slot A lato vfsd) E poi `fd_bind_remote_shadow` → `vfs_open` (alloca PTY slot B lato kernel). Le ioctls usano il cookie del shadow (slot B), la slave viene aperta su B, ma `write(master_fd)` usa `vfsd_proxy_write` su slot A che ha `slave_open=0` → -EIO. **Fix**: aggiunta `is_pty_path()` in `kernel/syscall.c`; `/dev/ptmx` e `/dev/pts/*` aprono sempre via kernel-direct VFS, bypassando vfsd.
- Selftest: `pty-core` con `PTYDEMO.ELF`. Suite: `57/57`.

### Stato operativo M11-05g / glibc Compatibility Shims

**File**: [user/glibc_compat.c](user/glibc_compat.c),
[kernel/elf_loader.c](kernel/elf_loader.c),
[toolchain/smoke/glibc_compat_demo.c](toolchain/smoke/glibc_compat_demo.c)

- `M11-05g` chiusa in `v1`.
- `GLIBC-COMPAT.SO` (`user/glibc_compat.c`): shared library PIC senza libc, esporta:
  - `__libc_start_main` — entry point glibc-style (chiama main + init/fini callbacks)
  - `gnu_get_libc_version` → `"2.38"`, `gnu_get_libc_release` → `"stable"`
  - `pthread_atfork` / `__register_atfork` — stub no-op (fork multi-thread non supportato)
  - `__cxa_thread_atexit_impl` — stub per distruttori TLS C++ (no-op)
  - `__stack_chk_guard` — canary data symbol con valore `0xdeadbeefcafebabe`
  - `__stack_chk_fail` — loop WFE (equivalente di abort senza libc)
- **ELF loader: `DT_GNU_HASH` support** (`kernel/elf_loader.c`):
  - aggiunto `gnu_hash_va` in `elf_dyn_info_t`; `DT_GNU_HASH = 0x6ffffef5` parsato in `elf_parse_dynamic`
  - check metadata accetta `gnu_hash_va != 0` come alternativa a `hash_va`
  - `sym_count` derivato da GNU hash quando `DT_HASH` assente: scan bucket max → walk chain fino a bit0=1
  - Sblocca caricamento `.so` glibc moderni che hanno solo `DT_GNU_HASH`
- **`libc.so.6` alias** (`elf_load_vfs_object_searched`): quando ricerca fallisce per librerie glibc note (`libc.so.6`, `libpthread.so.0`, `libm.so.6`, `libdl.so.2`, `librt.so.1`, `libresolv.so.2`), tenta `/GLIBC-COMPAT.SO` come fallback
- Demo: `GLIBCCOMPAT.ELF` via `glibccompat` comando boot
- Selftest: `glibc-compat`. Suite: `58/58`.

### Stato operativo M11-05d / ld-linux shim + library search paths

- `M11-05d` e' chiusa in `v1`: ELF loader risolve `PT_INTERP = /lib/ld-linux-aarch64.so.1` (e varianti musl/lib64) tramite alias automatico verso `/LD-ENLIL.SO` quando il file non esiste nel VFS.
- `LDINTDEMO.ELF` in initrd e' il test binario: PIE identico a `DYNDEMO.ELF` ma linked con `--dynamic-linker=/lib/ld-linux-aarch64.so.1`.
- Selftest di riferimento: `linux-ld-shim`; suite completa `55/55`.
- VFS aggiunto: `/lib/aarch64-linux-gnu` → bindfs → `/sysroot/lib/aarch64-linux-gnu` (per linux-compat-stage con layout Debian/Ubuntu multiarch).
- DT_NEEDED fallback search order: path originale → `/lib/aarch64-linux-gnu/<basename>` → `/usr/lib/<basename>` → `/usr/lib/aarch64-linux-gnu/<basename>`.
- Quando `linux-compat-stage` e' popolato (`make linux-compat-stage LINUX_ROOT_DIR=...`), `lib/ld-linux-aarch64.so.1` viene copiato dalla radice Linux reale e diventa accessibile via bindfs `/lib` → `/sysroot/lib`.
- Boot command interattivo: `ldintdemo`.

### Stato operativo M11-05a/b/c / bash-linux + epoll + SysV IPC

- `M11-05a` e' chiusa in `v1`: `bash-linux` statico funziona davvero.
- `M11-05b` e' chiusa in `v1`: `epoll_create1/ctl/pwait` sono disponibili sia nativamente sia nella tabella Linux AArch64.
- `M11-05c` e' chiusa in `v1`: System V shared memory e semaphore set sono disponibili sia nativamente sia nella tabella Linux AArch64.
- Smoke reale validato:
  ```sh
  /data/bash-linux -c 'echo ok'
  ```
  output:
  ```text
  ok
  ```
- Supporto critico aggiunto per arrivarci:
  - ABI Linux per-task scelta dal loader anche fuori dai mount `linux_compat`
  - `ET_EXEC` low-VA Linux caricato high e rialiasato low nel window user
  - `brk()` reale con backing high-VA + alias low-VA per heap glibc/malloc
  - `/proc/self/exe` e `/proc/self/fd/*` corretti lato procfs/VFS
  - `symlinkat`, `readlinkat`, `AT_REMOVEDIR`, `AT_SYMLINK_NOFOLLOW`
  - `utimensat`
  - `flock v1` con coda FIFO dedicata e deadlock detection best-effort
  - `epoll` bounded con `FD_TYPE_EPOLL`, `EPOLL_CTL_ADD/MOD/DEL`, `EPOLLET`, timeout e `EINTR`
  - System V IPC bounded con `shmget/shmat/shmdt/shmctl` e `semget/semop/semtimedop/semctl`
- Hardening ancora aperto ma non piu' bloccante per shell/tool Linux statici:
  - `mprotect` MMU completa
  - `prlimit64` write-side piu' ricca
  - alcuni stub Linux coerenti (`ptrace`, `setuid`, `sched_*affinity`, ecc.)
- Smoke interattivo validato: `runelf bash-linux` → prompt appare → `ls` → output → prompt torna. I bug che bloccavano questo path sono descritti nella sezione knowledge operativa bash-linux più sotto.
- Smoke `epoll` validato: `/EPOLLDEMO.ELF`, comando boot `epolldemo`, selftest `epoll-core`.
- Smoke `sysvipc` validato: `/SYSVIPC.ELF`, comando boot `sysvipcdemo`, selftest `sysv-ipc`.

### Knowledge operativa M8-08a/b/c (pipe, cwd/env, termios)

- Modello fd non più "valore per slot": ogni `fd_table[task][fd]` punta a `fd_object_t` condivisibile. `dup()`, `dup2()` e `fork()` devono condividere stesso oggetto con `refcount`, altrimenti `close()` rompe pipe/redirection.
- `syscall_task_cleanup()` obbligatoria su exit: rilascia fd, pipe e shadow state per task. Saltata → suite piena di writer/read-end fantasma.
- Pipe implementate in `kernel/syscall.c` con pool statico da 32 pipe e buffer da 4096 byte. EOF lato lettore arriva quando `writers == 0`; `fstat()` espone `S_IFIFO`.
- `O_NONBLOCK` utile soprattutto sul path pipe. Non è modello completo di I/O non bloccante generale su tutti i backend VFS.
- `getcwd()` / `chdir()` non devono mantenere stato parallelo nel kernel: source of truth è `vfsd`, coerente con namespace mount di `M9-04`.
- Environment bootstrap task lanciati con `elf64_spawn_path()`: `PATH=/bin:/usr/bin`, `HOME=/home/user`, `PWD=/`, `SHELL=/bin/arksh`, `TERM=vt100`, `USER=user`.
- Dopo `M8-08f`, login shell di default passa da `/bin/arksh`, che resta launcher/static bridge. Con `M11-03` + hardening libc hosted, port esterno reale di `arksh` ora builda host-side in `toolchain/build/arksh/arksh` e viene impacchettato automaticamente nell'`initrd` come `/bin/arksh.real`.
- Comando boot `arksh` deve provare solo shell reale `/bin/arksh.real` e dare errore chiaro se manca; fallback a `/bin/nsh` resta responsabilità del launcher/login path, non del comando esplicito.
- `boot_prepare_login_layout()` in `kernel/main.c` prepara layout persistente e file seed su `/data/home/user`: `.config/arksh/arkshrc` e `.local/state/arksh/history`.
- Gotcha operativo `M8-08f`: bind `/data/home -> /home` esiste e login shell fa `chdir("/home/user")`, ma accesso EL0 diretto al file history via `/home/user/.local/state/arksh/history` resta timing-sensitive. Selftest valido controlla quindi backing store kernel-side su `/data/home/...` e usa `/ARKSHBOOT.ELF` solo per verificare env/cwd/rc/bin-layout.
- Validazione `M8-08f`:
  - comando boot `arksh`
  - ELF `/ARKSHBOOT.ELF`
  - selftest `arksh-login`
  - suite completa `43/43`
- Gotcha pratico port hosted `arksh`: target CMake upstream builda anche benchmark/test (`arksh_perf_runner`) che richiedono API hosted extra (`wait4`, `rusage`, ecc.) non necessarie per usare la shell. Target supportato in EnlilOS deve buildare esplicitamente `--target arksh`.
- `M11-03` chiude `libdl` bootstrap reale:
  - syscall `dlopen/dlsym/dlclose/dlerror`
  - wrapper musl `<dlfcn.h>` + `libdl.a`
  - smoke `/MUSLDL.ELF`
  - selftest `musl-dlfcn`
  - limitazione nota: `dlclose()` rilascia handle ma non smappa ancora le pagine
- `execve()` deve preservare `envp` passato dal caller.
- `termios` è v1 minimale sulla console globale: canonical mode, raw mode, `isatty()`, `VINTR`, `VEOF`, `VERASE`, `VKILL`, `ISIG`, `ECHO/ECHOE`, `OPOST/ONLCR`.
- `VMIN/VTIME` non hanno ancora semantica POSIX completa e termios state non è per-open-file o per-pty. OK per bootstrap shell/REPL, non ancora per compat piena.
- `POSIXDEMO.ELF` e selftest `posix-ux` sono riferimento runtime per validare insieme: env bootstrap, `getcwd/chdir`, `pipe`, `dup`, `dup2`, `isatty`, `tcgetattr`, `tcsetattr`.

### Knowledge operativa — bash-linux interattivo (runelf + foreground + segnali)

**File**: [kernel/syscall.c](kernel/syscall.c), [kernel/sched.c](kernel/sched.c),
[include/sched.h](include/sched.h), [kernel/tty.c](kernel/tty.c),
[kernel/signal.c](kernel/signal.c), [kernel/mmu.c](kernel/mmu.c), [include/mmu.h](include/mmu.h)

#### `sys_linux_clone` — fork-path dispatch
- Bash usa `clone(SIGCHLD)` per forkare processi figli (senza `CLONE_THREAD`). `sys_linux_clone` passava tutto a `sys_clone` che richiede `CLONE_VM|CLONE_THREAD` → EINVAL.
- **Fix**: `sys_linux_clone` controlla `flags & CLONE_THREAD`; se assente → `sys_fork(args)`.

#### `runelf` — foreground TTY
- `runelf <path>` usava `elf64_spawn_path` direttamente senza chiamare `term80_activate` o impostare `BOOTCLI_MODE_TERM` → bash avviato ma non riceveva input da tastiera.
- **Fix**: `runelf` passa per `bootcli_launch_shell` che chiama `term80_activate` e setta la modalità terminale.

#### `sys_waitpid` — WCONTINUED + zombie reap
- Bash passa flag `WCONTINUED` (= 8) a `wait4`. Il kernel accettava solo `WNOHANG|WUNTRACED` → EINVAL.
  **Fix**: `options & ~(WNOHANG | WUNTRACED | 8U)` — WCONTINUED accettato come no-op.
- Zombie non veniva mai consumato dopo `waitpid`: il proc slot restava `waitable=1` per sempre.
  **Fix**: `sched_task_mark_reaped(t)` chiamata dopo il reap imposta `proc->waitable = 0` e `proc->in_use = 0`.
- **Attenzione**: `proc_of(t)` ritorna NULL se `in_use == 0`. Dopo `sched_task_mark_reaped`, nessuna API che usa `proc_of` deve essere chiamata su quel TCB.

#### `tty_is_background_current` — stale foreground pgrp
- Dopo che un figlio (es. `ls`) esce, `tty_foreground_pgid` punta al suo pgid (ora morto). Bash tenta di scrivere il prompt → `tty_is_background_current()` = true → SIGTTOU inviato → bash ha SIG_IGN → `tty_check_output_current` ritorna -EINTR → loop infinito.
- **Fix**: aggiunto controllo `!sched_task_has_pgrp(tty_session_sid, tty_foreground_pgid)` dentro `tty_is_background_current`. Se il pgid foreground non ha task vivi, il writer viene trattato come foreground.
- **Non fare**: aggiungere `tty_adopt_current_session()` a `tty_check_output_current`. Causa regressione in `jobctl-core`: i server di sistema (vfsd/netd) scrivono su stdout, `tty_adopt_current_session` può rubare `tty_session_sid` dalla sessione corretta assegnandola a quella dei server. `tty_adopt_current_session` appartiene solo al path `tty_read`.

#### Signal trampoline Linux ABI
- La pagina trampoline a `MMU_USER_SIGTRAMP_VA = MMU_USER_LIMIT - 0x1000` conteneva solo `movz x8, #19; svc #0; brk #0` (EnlilOS `SYS_SIGRETURN = 19`). Processi Linux ABI dispatchano via `linux_syscall_table` → syscall 19 non esiste lì → ENOSYS → cade su `brk #0` → SIGILL loop.
- **Fix**: seconda trampoline a `MMU_USER_LINUX_SIGTRAMP_VA = MMU_USER_SIGTRAMP_VA + 12` con `movz x8, #139` (Linux `NR_rt_sigreturn = 139`). La pagina fisica contiene entrambe. `signal_deliver_to_current` seleziona la VA in base a `sched_task_abi_mode(current_task)`.
- Encoding AArch64: `movz x8, #N` = `0xD2800000 | (N << 5) | 8`. Per N=19: `0xD2800268`; per N=139: `0xD2801168`.

### Knowledge operativa — kbd ring buffer OOB (62° char freeze)

**File**: [drivers/keyboard.c](drivers/keyboard.c)

- `kbd_event_buf[KBD_EVENT_BUF_SIZE]` con `KBD_EVENT_BUF_SIZE = 64`. Indici `kbd_event_head`/`kbd_event_tail` erano `uint8_t` incrementati con `++` nudo → wrap naturale a 256, non a 64.
- Al 64° evento, `kbd_event_head` saliva a 64 e scriveva fuori dal buffer. Layout BSS (analizzato con `nm`): offset 0x5d8 dopo il buffer è `kbd_backend`. Il field `unicode` (offset 8 nell'evento) atterrava su `kbd_backend`, sovrascrivendo `KBD_BACKEND_VIRTIO = 1` con il valore Unicode del tasto premuto (≥ 32). `keyboard_getc()` cadeva nel fallback UART → ritornava -1 per sempre.
- **Fix**: aggiunto `% KBD_EVENT_BUF_SIZE` in `kbd_event_full`, `kbd_event_push` e `kbd_event_pop`.
- Il fix display-side (`BOOTCLI_INPUT_MAX` + right-scroll) era corretto ma secondario; il freeze era esclusivamente OOB.

### Knowledge operativa — prlimit64 (SYS_PRLIMIT64 = 212)

**File**: [include/rlimit.h](include/rlimit.h), [include/sched.h](include/sched.h),
[kernel/sched.c](kernel/sched.c), [kernel/syscall.c](kernel/syscall.c),
[toolchain/enlilos-musl/src/resource.c](toolchain/enlilos-musl/src/resource.c),
[toolchain/enlilos-musl/include/sys/resource.h](toolchain/enlilos-musl/include/sys/resource.h)

- `rlimit64_t` e costanti `RLIMIT_*` / `RLIM64_INFINITY` in `include/rlimit.h` standalone. Motivo: `sched.h` non può includere `syscall.h` (dipendenze circolari); `syscall.h` fa `#include "rlimit.h"` per evitare ridefinizioni.
- `MAX_FD = 64` spostato da `kernel/syscall.c` (locale) a `include/syscall.h` (pubblico): `sched.c` ne ha bisogno per `RLIMIT_NOFILE` default.
- `sched_proc_ctx_t` contiene `rlimit64_t rlimits[RLIMIT_NLIMITS]`. Defaults impostati in `proc_rlimit_defaults()`, ereditati via `memcpy` in `sched_task_fork_user()`.
- Default notevoli: `RLIMIT_STACK = 8MB cur / ∞ max`, `RLIMIT_CORE = 0/0`, `RLIMIT_NOFILE = MAX_FD/MAX_FD`, `RLIMIT_NPROC = SCHED_MAX_TASKS`.
- `sys_prlimit64()`: `pid=0` → processo corrente; ricerca per `tgid` con `sched_task_find()`; `old_uva != 0` → copia limit out; `new_uva != 0` → valida (`cur ≤ max` salvo `max = ∞`) poi aggiorna.
- Toolchain musl: `resource.c` wrappa `user_svc4(SYS_PRLIMIT64, ...)`. `prlimit/getrlimit/setrlimit` convertono tra `rlimit` (32-bit `rlim_t`) e `rlimit64`.
- Selftest non aggiunto (nessun test case esplicito per `prlimit64`); suite 49/49 passa invariata.

### Knowledge operativa — shutdown/poweroff (SYS_REBOOT = 213)

**File**: [include/psci.h](include/psci.h), [kernel/psci.c](kernel/psci.c),
[include/shutdown.h](include/shutdown.h), [kernel/shutdown.c](kernel/shutdown.c),
[toolchain/enlilos-musl/include/sys/reboot.h](toolchain/enlilos-musl/include/sys/reboot.h),
[toolchain/enlilos-musl/src/reboot.c](toolchain/enlilos-musl/src/reboot.c),
[toolchain/smoke/poweroff.c](toolchain/smoke/poweroff.c)

- **PSCI** (`kernel/psci.c`): `psci_system_off()` / `psci_system_reset()` via **`hvc #0`** con function ID `0x84000008` / `0x84000009`. QEMU virt senza `-machine virt,secure=on` non ha EL3: PSCI esposto via HVC (EL2), non SMC. Usare `smc #0` causa EC=0x00 (Unknown) trapped a EL1 → crash. Fallback `wfe` loop se HVC non ritorna.
- **Sequenza graceful** (`kernel/shutdown.c`): `vfs_sync()` → `blk_flush_sync()` → SIGTERM tutti task user-space → wait 2s → SIGKILL superstiti → `gic_disable_irqs()` → PSCI/halt. CRITICO: `vfs_sync` PRIMA di SIGTERM/sched_yield, altrimenti i task user-space (vfsd/blkd/nsd) corrompono mount ops durante il drain → PC alignment fault in `vfs_sync`.
- **SYS_REBOOT = 213**: `cmd = REBOOT_CMD_POWER_OFF (0x4321FEDC)`, `REBOOT_CMD_RESTART (0x01234567)`, `REBOOT_CMD_HALT (0xCDEF0123)`. Dispatcher chiama `shutdown_system()` che è `noreturn`.
- **Bootcli**: comandi `poweroff` (= `shutdown`), `reboot`, `halt` chiamano `shutdown_system()` direttamente da kernel, dopo aver fatto `bootcli_render()` per mostrare messaggio finale.
- **musl**: `reboot(cmd)` via `user_svc1(SYS_REBOOT, cmd)`. Header `<sys/reboot.h>` con `RB_POWER_OFF`, `RB_AUTOBOOT`, `RB_HALT_SYSTEM`.
- **`/sbin/poweroff`**: ELF musl bootstrap installato anche come `/sbin/reboot` e `/sbin/halt`. `poweroff -r` → reboot, `poweroff -h` → halt.
- **Gotcha vfs_sync**: `vfs_ptr_sane()` in `vfs_sync()` skippa mount con `ops` o `ops->sync` fuori range kernel (`0x40000000–0xC0000000`) o non allineati → guard difensivo contro corruzione futura.
- **Gotcha IRQ**: `gic_disable_irqs()` disabilita solo IRQ AArch64 (`msr daifset, #2`), non il timer. Non fare `sched_yield()` dopo `gic_disable_irqs()` altrimenti il timer non scatta e si rimane bloccati.

### Knowledge operativa arksh runtime (M8-08f/g)

- `sys_mmap` ha limite pagine per singola chiamata alzato a **1024 pagine (4MB)** (era 256 = 1MB). Motivo: `sizeof(ArkshAst) ≈ 1.88MB = 460 pagine`; vecchio limite faceva fallire ogni `calloc(1, sizeof(*ast))` → "unable to allocate parser state".
- `vsnprintf` nel bootstrap musl originariamente non gestiva `%z`/`%j`. Line editor arksh usa `fprintf(stdout, "\033[%zuD", move_left)` per spostare cursore; senza fix, formato `%zu` veniva emesso letteralmente come testo, producendo sequenze ANSI malformate visibili a schermo. Fix: aggiunto blocco `if (*fmt == 'z' || *fmt == 'j') { long_flag = 2; fmt++; }` in `vsnprintf`.
- **Bug critico: mmap_base bump allocator vs ELF text**.
  `mmu_map_user_anywhere` mantiene `mmap_base` che avanza monotonicamente; `munmap` libera pagine fisiche ma **non fa tornare indietro `mmap_base`**. ELF arksh caricato a `0x7FC1000000` ma `mmap_base` partiva da `MMU_USER_BASE = 0x7FC0000000` — solo 16MB di gap. Dopo ~4000 allocazioni da 1 pagina (qualche centinaio per comando), `mmap_base` raggiungeva testo ELF e lo sovrascriveva con pagine azzerate: `vsnprintf` produceva garbage, parser entrava in stati inconsistenti.
  **Fix**: `mmu_space_set_mmap_base(space, image_hi + PAGE_SIZE)` chiamato in `elf_load_common` dopo il loading → `mmap_base` parte sopra l'ultimo segmento caricato (~`0x7FC10AA000` per arksh), non dal fondo del VA window.
  API aggiunta: `mmu_space_set_mmap_base()` in `kernel/mmu.c` + `include/mmu.h`.
- Attenzione: CMake **non rilinka** automaticamente se cambia solo `libc.a` (non i sorgenti `.c`). Dopo ogni modifica al bootstrap musl, fare `rm toolchain/build/arksh/arksh && make arksh-build` per forzare il relink.

### Knowledge operativa M8-08g (layout tastiera `us`/`it`)

- `keyboard_getc()` resta path legacy consumato da console/TTY, ma ora byte arrivano da layer eventi più ricco: `keycode + modifiers + keysym + unicode`. Per caratteri non ASCII il driver emette direttamente UTF-8.
- v1 usa layout attivo globale per console corrente. Non c'è ancora stato separato per TTY multipla o per-sessione shell; per architettura attuale single-console intenzionale e sufficiente.
- Layout integrati oggi: `us` e `it`. Tabella `it` supporta anche casi minimi per uso locale: `à è é ì ò ù`, `@`, `#`, `£`, `§`, `°`, parentesi quadre/graffe, `AltGr` e tasti morti basilari.
- Persistenza layout:
  - bootstrap RO in `/etc/vconsole.conf`
  - override persistente RW in `/data/etc/vconsole.conf`
  - ordine load al boot: `/data/etc/vconsole.conf` prima, fallback `/etc/vconsole.conf`
- Comandi utente bootstrap: `/usr/bin/loadkeys` e `/usr/bin/kbdlayout`. Non devono includere header kernel del repo nel path di build musl, altrimenti rompono header libc bootstrap. Usano `user_svc.h` + numeri syscall dedicati, restano isolati dal namespace header kernel.
- Bug reale da packaging: aggiungere keymap e utility ha portato initrd a 70 entry. Parser CPIO aveva `INITRD_MAX_ENTRIES = 64`, rootfs montava come `initrd-error`. Fix: alzato a 128.
- Selftest di riferimento: `kbd-layout`; suite completa valida ora `47/47`.

### Knowledge operativa M10-01 (virtio-net + netd bootstrap)

- Driver in `drivers/net.c`, usa solo trasporto `virtio-mmio` moderno (`version == 2`) con device ID `1` (`VIRTIO_NET_DEVICE_ID`).
- Feature negoziate in v1: `VIRTIO_F_VERSION_1`, `VIRTIO_NET_F_MAC`, `VIRTIO_NET_F_STATUS`. Non assumere offload avanzati o mergeable buffers.
- Modello attuale **raw Ethernet only**:
  - queue `RX` con buffer DMA preallocati
  - queue `TX` con submit sincrono bounded
  - ring software statico per consegnare frame a `net_recv()`
  - nessun IPv4/ARP/TCP nel kernel
- API kernel-side esposta:
  - `net_init()`
  - `net_is_ready()`
  - `net_get_info()`
  - `net_send()`
  - `net_recv()`
  - `net_selftest_run()`
- `netd` è server EL0 bootstrap, non ancora stack di rete completo. Usa syscall `SYS_NET_BOOT_SEND`, `SYS_NET_BOOT_RECV`, `SYS_NET_BOOT_INFO` e si limita a:
  - pubblicare MAC/link
  - drenare frame
  - fungere da ponte per `M10-02`
- Boot crea e ribinda esplicitamente porta microkernel `net`. Pattern speculare a `vfsd`/`blkd`: se porta esiste ma owner non riassegnato al task EL0 giusto, syscall bootstrap rifiutano richieste.
- Target `make run`, `make run-fb`, `make run-gpu`, `make run-blk` e `make test` includono ormai `-netdev user,id=net0 -device virtio-net-device,netdev=net0`. Se driver non sale, verificare prima command line QEMU.
- Selftest di riferimento: `net-core`. Valida:
  - presenza porta `net`
  - spawn corretto `/NETD.ELF`
  - stato `net_is_ready()`
  - `net_selftest_run()` con TX minimo e contatori
- Gotcha chiuso durante bring-up: dopo cambio default tastiera a `it`, `vfs-rootfs` falliva ancora cercando `KEYMAP=us`. Non era regressione rete, ma aspettativa selftest stantia.

### Knowledge operativa M10-02 (TCP/IP stack user-space)

**File**: [user/net_stack.h](user/net_stack.h), [user/net_stack.c](user/net_stack.c),
[user/netd.c](user/netd.c)

- Stack freestanding, gira dentro `netd` (EL0), senza libc.
- `net_stack_init(mac, out_fn)` riceve MAC dal driver kernel (via `SYS_NET_BOOT_INFO`) e puntatore a funzione per TX (`SYS_NET_BOOT_SEND`).
- `net_stack_input(frame, len)` chiamato nel loop di ricezione `netd` per ogni frame ricevuto da `SYS_NET_BOOT_RECV`.
- `net_stack_send_garp()` va chiamato una volta dopo `net_stack_init`: invia Gratuitous ARP che fa incrementare `tx_packets` nel driver kernel — prova funzionale usata dal selftest `net-stack`.
- Selftest `net-stack` aspetta fino a 2 s che `net_get_info().tx_packets > 0` e verifica che `NETD.ELF` sia > 4096 byte (net_stack.o linkato).
- Protocolli supportati in v1: Ethernet, ARP (cache 8 entry), IPv4, ICMP echo reply, UDP (4 socket con callback), TCP passivo (4 connessioni, SYN→SYN+ACK→ESTABLISHED→FIN).
- IP statica QEMU SLIRP: `10.0.2.15/24`, gateway `10.0.2.2`.
- Byteorder: tutti campi protocollo usano helper `rd16/wr16/rd32/wr32` (byte-by-byte BE); funzioni `ns_htons/ns_htonl` convertono valori host→network.
- `net_stack.o` non è standalone: va linkato esplicitamente in `netd.elf` tramite rule Makefile esplicita (`user/netd.elf: user/netd.o user/net_stack.o`).
- Pool sizes: `NET_STACK_ARP_ENTRIES=8`, `NET_STACK_UDP_SOCKETS=4`, `NET_STACK_TCP_CONNS=4`, `NET_STACK_TCP_RXBUF=2048`.
- v1 senza retransmit TCP: QEMU/SLIRP su loopback è lossless.
- M10-03 chiude la BSD socket API `v1`: syscall 200–211 cablate in kernel/libc bootstrap, `AF_INET` loopback-only, `SOCK_STREAM` + `SOCK_DGRAM`, `SOCK_NONBLOCK`, `SOCK_CLOEXEC`, `MSG_DONTWAIT`, `SO_REUSEADDR`, `SO_ERROR`, `shutdown`.

### Knowledge operativa M10-03 (BSD socket API v1)

- Implementazione in `kernel/sock.c` con pool globale statico da `32` socket (`SOCK_MAX_GLOBAL`).
- Perimetro `v1` volutamente stretto:
  - solo `AF_INET`
  - solo loopback `127.0.0.1`
  - destinazioni esterne → `-ENETUNREACH`
  - `SOCK_STREAM` + `SOCK_DGRAM`
- `connect()` TCP crea un peer kernel-side e lo inserisce nella accept queue del listener; non passa ancora attraverso il backend IP/TCP di `netd`.
- `send/recv/sendto/recvfrom` supportano path non bloccante sia via `SOCK_NONBLOCK` sia via `MSG_DONTWAIT`; lo stato `O_NONBLOCK` dell'fd va sincronizzato verso `sock_t->flags` con `sock_sync_fd_flags()`.
- `SOCK_CLOEXEC` viene tradotto subito in `FD_CLOEXEC` sull'entry fd; `accept()` `v1` non espone ancora `accept4()`.
- Socket demo di riferimento: `/SOCKDEMO.ELF`, comando boot `socketdemo`. Verifica:
  - `SO_REUSEADDR`
  - echo TCP su `127.0.0.1:7070`
  - UDP loopback su `127.0.0.1:7071`
- Selftest di riferimento: `socket-api`; stato validato insieme al resto della suite a `47/47`.

### Knowledge operativa M9-04 (namespace + mount dinamico)

- `vfsd` è point of truth per vista filesystem dei task EL0: `cwd`, mount table privata e risoluzione path vivono lato server, non nel kernel.
- Nuovi syscall `chdir/getcwd`, `mount/umount`, `unshare(CLONE_NEWNS)` e `pivot_root()` devono passare tramite `vfsd`; non introdurre bypass kernel-side che risolvano path direttamente.
- `execve()` / `spawn()` e path shadow fd usati da `mmap/msync` devono sempre risolvere path attraverso `vfsd`, altrimenti rompono namespace privati.
- Ereditarietà namespace dopo `fork()` implementata in `vfsd` usando `ipc_message_t.sender_tid` + `SYS_VFS_BOOT_TASKINFO`: al primo accesso del figlio il server copia `ns_id` e `cwd` dal parent.
- `pivot_root()` deve ribasare vecchio root nella nuova vista. Bug reale risolto: con `pivot_root("/mnt", "/mnt/oldroot")`, vecchio root va esposto a `/oldroot`, non lasciato a `/mnt/oldroot`.
- `NSDEMO.ELF` e selftest `vfs-namespace` verificano path completo: `unshare`, bind mount, `chdir/getcwd`, mount `procfs`, `fork()` con ereditarietà, `pivot_root()` e visibilità di `/oldroot/BOOT.TXT`.
- Log `boot_open fail` di `vfsd` su file di probe assenti durante demo (`SIGREADY.TXT`, `JOBREADY.TXT`, `/data/proc/...`) non indicano necessariamente bug: spesso attesi nel percorso di test.
- User-space freestanding: non introdurre dipendenze implicite da libc. In `vfsd` usare helper locali (`vfsd_memcpy`, `vfsd_bzero`, ecc.), non assumere che `memcpy` venga risolta dal linker.

### Limiti noti v1 M9-04

- Backend filesystem reali restano bootstrap kernel-side dietro `SYS_VFS_BOOT_*`.
- `vfsd` non fa ancora garbage collection dello stato client/namespace dei PID terminati.
- Semantica `pivot_root()` sufficiente per bootstrap e test, non ancora modello completo da container/runtime avanzato previsto in `M11-07`.

---

## Bug storici noti (risolti, non ripetere)

### Re-entrant schedule() race → PC=0 crash
`irq_restore()` prima di `sched_context_switch()` apre finestra dove timer IRQ chiama `schedule()` rientrante, sovrascrivendo `next->sp` con frame sullo stack di `prev`. Al ripristino: x30=0 → `ret` salta a PC=0 → Instruction Abort.
**Risolto**: IRQ disabilitati durante switch, poi `msr daifclr, #2` (non `irq_restore`).

### Direct priority mutation → double-insert in run queue
Impostare `task->priority = X` mentre task è READY corrompe run queue (task finisce in due bucket simultaneamente). Vedere sezione scheduler sopra.
**Risolto**: usare sempre `sched_task_donate_priority()`.

### ksem holder starvation a PRIO_NORMAL
Holder a PRIO_NORMAL=128 starved dall'hog (stesso livello, FIFO precedente). PI non aiuta: donazione arriva solo DOPO che waiter entra in wait, ma holder non riesce mai ad acquisire lock perché hog gira in CPU-bound loop alla stessa prio.
**Risolto**: holder creato a PRIO_HIGH=32.

### mmap_base bump allocator sovrascrive testo ELF → garbled output + crash parser
`mmu_map_user_anywhere` usa bump allocator per VA utente; `munmap` libera pagine fisiche ma non fa retrocedere `mmap_base`. Con `MMU_USER_BASE = 0x7FC0000000` e ELF caricato a `0x7FC1000000` il gap è solo 16MB = 4096 pagine da 4KB. Dopo ~4000 allocazioni (cumulativamente su più comandi shell) bump allocator raggiunge testo ELF e lo sovrascrive con pagine azzerate → `vsnprintf` produce garbage (`ls]K` a schermo), parser arksh entra in stati inconsistenti → "unable to allocate parser state" al 3° comando.
**Risolto**: in `elf_load_common` (`kernel/elf_loader.c`) dopo loading: `mmu_space_set_mmap_base(space, image_hi + PAGE_SIZE)`.
API `mmu_space_set_mmap_base()` aggiunta in `kernel/mmu.c` + `include/mmu.h`.
