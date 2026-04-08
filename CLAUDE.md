# EnlilOS — Guida per Claude

Microkernel AArch64 in stile GNU Hurd. Questo file cattura la conoscenza
architetturale necessaria per lavorare sul codebase senza dover rileggere
tutto ogni sessione.

---

## Target e build

- **Architettura**: AArch64 (cortex-a72), QEMU virt, 512MB RAM
- **Toolchain**: `aarch64-elf-gcc`, `aarch64-elf-ld`
- **Output kernel**: `enlil.elf` / `enlil.bin`
- **Output selftest**: `enlil-selftest.elf` (suite separata, flag `-DENLILOS_SELFTEST=1`)
- **Comandi**:
  - `make` — build normale
  - `make test` — build selftest + lancio QEMU (con timeout via SIGKILL al termine)
  - `make run` — boot kernel normale in QEMU
- **Il `Killed: 9` finale di `make test` è normale** — il Makefile invia SIGKILL a QEMU
  dopo che il kernel stampa "Sistema fermo". Non è un crash.
- Il disco `disk.img` viene lockato da QEMU: se una sessione rimane appesa,
  la successiva fallisce con "Failed to get write lock". Usare `pkill -f qemu-system-aarch64`.

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
**NON modificare il layout del TCB** — `sched_switch.S` dipende da `sp` a offset 0.

### Run queue
- `rq_head[256]` / `rq_tail[256]`: FIFO singly-linked per priorità
- `ready_bitmap[4]`: 256 bit, `bitmap_find_first()` → O(1) via CTZ
- `rq_push(t)`: aggiunge a coda del bucket `eff_prio_of(t)`. **Pericolo doppio-push**: corrupe la catena.
- `rq_pop(p)`: estrae dalla testa, setta `t->next = NULL`
- `rq_remove(t, p)`: scansione lineare, silent fail se non trovato

### Priority Inheritance (PI)
- `donated_priority[slot]`: per-task, monotonicamente decrescente (0xFF = nessuna donazione)
- `eff_prio_of(t) = min(t->priority, donated_priority[slot])`
- `sched_task_donate_priority(t, prio)`: aggiorna donated_priority. Se t è READY → fa `rq_remove(t, old_eff)` + `rq_push(t)`. Se t è BLOCKED o RUNNING → non tocca la run queue.
- `sched_task_clear_donation(t)`: resetta donated_priority a 0xFF.

### PERICOLO: direct priority mutation
**Non fare MAI** `task->priority = X` mentre il task è in READY nella run queue.
Il task è nel bucket `eff_prio_of()` calcolato PRIMA della mutazione. Se si cambia
`priority` direttamente, `eff_prio_of` cambia ma il task è ancora nel vecchio bucket →
`rq_remove(t, nuovo_eff)` fallisce silenziosamente → `rq_push` aggiunge nel nuovo bucket
senza rimuovere dal vecchio → task in **due bucket contemporaneamente** → corruzione
della run queue → crash.
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
IRQ rimangono **disabilitati** durante `sched_context_switch`. NON usare
`irq_restore(flags)` prima del context switch: aprirebbe una finestra dove
un timer IRQ chiamerebbe `schedule()` rientrante, corrompendo `next->sp` →
crash (PC=0, x30=0). Dopo il context switch si usa `msr daifclr, #2`
(non `irq_restore`) per riabilitare incondizionatamente gli IRQ — perché il
task ripreso potrebbe avere flags con I=1 (era bloccato dentro un syscall
handler dove l'HW imposta DAIF.I=1 all'ingresso eccezione).

### sched_block vs sched_yield
- `sched_block()`: state=BLOCKED, task non re-inserito in coda. Richiede `sched_unblock()` esplicito.
- `sched_yield()`: task rimane READY, re-inserito in coda dopo il reschedule.
- `sched_unblock(t)`: agisce solo se `t->state == TCB_STATE_BLOCKED`.

### Preemption hardware
Dopo ogni IRQ in `vectors.S`: se `need_resched=1` → `schedule()`. Il timer
chiama `sched_tick()` ogni 1ms che decrementa il quantum e setta `need_resched`.

---

## Syscall ABI

**File**: [include/syscall.h](include/syscall.h), [kernel/syscall.c](kernel/syscall.c)

- Numero syscall in **x8**, argomenti in **x0–x5**, valore di ritorno in **x0**
- Valori negativi = `-errno`
- Tabella `syscall_table[256]` indicizzata direttamente (O(1))
- `ERR(e)` = `(uint64_t)(-(int64_t)(e))` — macro per costruire codici d'errore

### Assegnazioni stabili
```
1–20    base POSIX (write/read/exit/open/close/fstat/mmap/munmap/brk/execve/
         fork/waitpid/clock_gettime/getdents/task_snapshot/spawn/
         sigaction/sigprocmask/sigreturn/yield)
60–64   capability (cap_alloc/cap_send/cap_revoke/cap_derive/cap_query)
80–84   mreact (subscribe/wait/cancel/subscribe_all/subscribe_any)
85–94   ksem (create/open/close/unlink/post/wait/timedwait/trywait/getvalue/anon)
95–99   kmon (create/destroy/enter/exit/wait)
100–119 ANE
110–111 kmon_signal / kmon_broadcast
120–139 GPU
134     kill
140–142 IPC (port_lookup/ipc_wait/ipc_reply)
150–155 VFS boot (vfs_boot_open/read/write/readdir/stat/close)
156–159 BLK boot (blk_boot_read/write/flush/sectors) — solo owner porta "block"
```

### Wrapper EL0 (`include/user_svc.h`)
Per programmi user-space usare sempre `user_svc0..6()` e `user_svc_exit()`.
Non scrivere inline assembly SVC nei file `.c` dei demo.

---

## Semafori kernel (`ksem`)

**File**: [kernel/ksem.c](kernel/ksem.c), [include/ksem.h](include/ksem.h)

- Pool statico: `ksem_pool[64]`, `ksem_waiters[64]`, `ksem_refs[512]`
- Wait con `sched_yield()` (busy-wait cooperativo), non `sched_block()`
- PI: `ksem_update_pi_locked()` → `sched_task_donate_priority(owner_hint, best_waiter_prio)`
- Flag `KSEM_RT`: attiva PI e `owner_hint` tracking
- `ksem_task_cleanup(task)`: chiamato da `sched_task_exit_with_code()` all'uscita del task

### Pattern selftest ksem
- Il **holder** deve avere priorità **PRIO_HIGH** (non PRIO_NORMAL).
  Con PRIO_NORMAL verrebbe starved dall'hog task (stesso livello, inserito prima in FIFO).

---

## Monitor kernel (`kmon`)

**File**: [kernel/kmon.c](kernel/kmon.c), [include/kmon.h](include/kmon.h)

- Semantica Mesa-style (condition variable + mutex integrato)
- `kmon_wait_current()`: usa `sched_yield()`, non `sched_block()`
- `kmon_refresh_owner_locked()`: usa `sched_task_donate_priority()` — **mai** `task->priority =`
- Priority ceiling per monitor RT: donazione automatica al momento di `kmon_enter`
- `kmon_task_cleanup(task)`: chiamato a exit

---

## Capability System (`cap`)

**File**: [kernel/cap.c](kernel/cap.c), [include/cap.h](include/cap.h)

- `cap_pool[256]`: pool globale capability attive/libere
- `cap_table[SCHED_MAX_TASKS][64]`: per-task, array di token posseduti
- Token 64-bit generato con `CNTPCT_EL0 ^ pid ^ cap_salt` (non indovinabile)
- `cap_alloc_kernel(pid, type, rights, object)`: API interna per il kernel
- Syscall: `cap_alloc(60)`, `cap_send(61)`, `cap_revoke(62)`, `cap_derive(63)`, `cap_query(64)`
- Solo l'`owner_pid` può revocare; `cap_derive` non può aggiungere diritti rispetto al padre

---

## VFS server user-space (`vfsd`)

**File**: [user/vfsd.c](user/vfsd.c)

- Task EL0 con pid=4, porta IPC "vfs"
- Usa syscall `vfs_boot_*` (150–155) per accedere al VFS kernel-side
- Selftest `vfsd-core` verifica che il server risponda correttamente a open/read/stat
- Bootstrap: spawned da `kernel/main.c` come primo task user-space

---

## Block server user-space (`blkd`)

**File**: [user/blkd.c](user/blkd.c), [include/blk_ipc.h](include/blk_ipc.h)

- Task EL0 con pid=5, porta IPC "block", priorità PRIO_LOW
- Usa syscall `blk_boot_*` (156–159) per accedere al driver virtio-blk kernel-side
- Guard `blk_srv_owner_ok()`: solo il processo owner della porta "block" può chiamarle
- IPC payload compatto: `blkd_request_t` (16 B) + `blkd_response_t` (16 B)
  → dati I/O non transitano nell'IPC, blkd usa buffer statico interno (`blkd_io_buf`)
- Selftest `blkd-core` verifica: porta registrata, owner user-space, `blk_is_ready()`, capacità
- Bootstrap: `boot_launch_blkd()` in `kernel/main.c`, chiamato dopo `boot_launch_vfsd()`

### Pattern blk_boot_* vs vfs_boot_*
Identico al pattern vfsd: le syscall `blk_boot_*` sono accessibili **solo** al task
che possiede la porta "block" (verificato con `mk_port_lookup("block")->owner_tid`).
Questo evita escalation di privilegi: un processo EL0 arbitrario non può leggere il disco raw.

---

## mreact (Reactive Memory Subscriptions)

**File**: [kernel/mreact.c](kernel/mreact.c), syscall 80–84

- Permette a un task di sottoscrivere notifiche su range di indirizzi fisici
- `mreact_subscribe(addr, len, flags)` → handle
- `mreact_wait(handle, timeout)` → blocca fino a modifica nell'area
- Usato internamente dal VFS per notifiche di I/O

---

## Exception vector e IRQ

**File**: [boot/vectors.S](boot/vectors.S)

- Ogni entry della vector table fa 5 istruzioni (sub/stp/mov/mov/b)
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
- `ERET` al termine ripristina `SPSR_EL1` che contiene DAIF del contesto interrotto
  (incluso I=0 → IRQ abilitati per task in esecuzione normale)

---

## Memoria fisica e virtuale

- **PMM**: buddy allocator + slab. `phys_alloc_page()` → pagina 4KB
- **MMU**: 3 livelli, pagine 4KB
  - L1[0]: MMIO 0x00000000–0x3FFFFFFF (Device-nGnRnE)
  - L1[1]: RAM  0x40000000–0x7FFFFFFF (Normal WB)
- **Kernel heap**: named typed caches (`task_cache` 64B, `port_cache` 64B, `ipc_cache` 512B)
- Stack task kernel: 4KB (1 pagina), allocata con `phys_alloc_page()`
- `TASK_STACK_SIZE = 4096`, `SCHED_MAX_TASKS = 32`

---

## ELF loader e user-space

- **Static loader**: `elf64_spawn_path()`, `elf64_load_from_path()`
- **Dynamic loader**: `ld_enlil.so` in `user/ld_enlil.c` (custom, non musl)
- I binari ELF user-space sono embedded nell'immagine kernel tramite `*.embed.o`
  e accessibili via initrd CPIO
- ELF user entrano a EL0 via `sched_enter_user()` (assembly in sched_switch.S)
- `sched_task_bootstrap()` decide kernel vs user vs resume-from-frame

---

## IPC sincrono

**File**: [kernel/microkernel.c](kernel/microkernel.c)

- Rendez-vous sincrono: client blocca fino alla risposta del server
- Priority donation: server eredita priorità del client più urgente
- Budget per-porta: `mk_port_set_budget()` per latency tracking
- Syscall: `port_lookup(140)`, `ipc_wait(141)`, `ipc_reply(142)`

---

## Self-test suite

**File**: [kernel/selftest.c](kernel/selftest.c)

20 test case in ordine:
1. `vfs-rootfs` — mount initrd, readdir, stat file
2. `vfs-devfs` — devfs /dev/stdin /dev/stdout
3. `ext4-core` — mount rw ext4, journal replay
4. `vfsd-core` — VFS server user-space
5. `blkd-core` — Block server user-space (porta "block", blk_is_ready, capacità)
6. `elf-loader` — carica ELF statico in EL0
7. `init-elf` — parse INIT.ELF
8. `nsh-elf` — parse NSH.ELF
9. `execve` — execve() completo
10. `exec-target` — target execve con EXEC2.ELF
11. `elf-dynamic` — carica ELF PIE con ld_enlil.so
12. `fork-cow` — fork() + COW + output su file ext4
13. `signal-core` — sigaction/sigreturn/kill
14. `mreact-core` — reactive subscriptions
15. `cap-core` — capability alloc/query/derive/revoke
16. `ksem-core` — semafori RT + priority inheritance
17. `kmon-core` — monitor + PI ceiling
18. `ipc-sync` — IPC sincrono + priority donation
19. `kdebug-core` — crash reporter + ksymtab
20. `gpu-stack` — VirtIO-GPU scanout + renderer 2D

### Helper macro
```c
ST_CHECK(case_name, cond, detail)  // ritorna -1 se !cond
```

### Pattern test con task ausiliari
Quando si creano task ausiliari (holder/hog/waiter):
- Il **holder** va a `PRIO_HIGH` se deve acquisire una risorsa prima di essere starved
- Non mutare mai `task->priority` direttamente — usare `sched_task_donate_priority()`
- Per aspettare un task: loop su `timer_now_ms()` con `sched_yield()` e deadline

---

## Milestone completate (stato 2026-04-09)

Tutto il backlog 1 (M1–M7) e backlog 2 fino a M9-04 sono completi in forma v1.
Il run di riferimento e':

```text
SUMMARY total=24 pass=24 fail=0
```

**Prossime priorità**:
1. M8-08a pipe() + dup/dup2
2. M11-01 musl libc
3. M10-01 VirtIO network driver
4. M8-08 termios/getcwd/env e arksh default shell
5. M11-02 POSIX Threading (pthread)

### Knowledge operativa M9-04 (namespace + mount dinamico)

- `vfsd` e' il punto di verita' per la vista del filesystem dei task EL0: `cwd`, mount table privata
  e risoluzione dei path vivono lato server, non nel kernel.
- I nuovi syscall `chdir/getcwd`, `mount/umount`, `unshare(CLONE_NEWNS)` e `pivot_root()`
  devono passare tramite `vfsd`; non introdurre bypass kernel-side che risolvano path direttamente.
- `execve()` / `spawn()` e il path shadow dei file descriptor usati da `mmap/msync` devono sempre
  risolvere il path attraverso `vfsd`, altrimenti rompono i namespace privati.
- L'ereditarieta' dei namespace dopo `fork()` e' implementata in `vfsd` usando `ipc_message_t.sender_tid`
  + `SYS_VFS_BOOT_TASKINFO`: al primo accesso del figlio il server copia `ns_id` e `cwd` dal parent.
- `pivot_root()` deve ribasare il vecchio root nella nuova vista. Esempio reale del bug risolto:
  con `pivot_root("/mnt", "/mnt/oldroot")`, il vecchio root va esposto a `/oldroot`, non lasciato
  a `/mnt/oldroot`.
- `NSDEMO.ELF` e il selftest `vfs-namespace` verificano il path completo: `unshare`, bind mount,
  `chdir/getcwd`, mount `procfs`, `fork()` con ereditarieta', `pivot_root()` e visibilita' di `/oldroot/BOOT.TXT`.
- I log `boot_open fail` di `vfsd` su file di probe assenti durante i demo (`SIGREADY.TXT`, `JOBREADY.TXT`,
  `/data/proc/...`) non indicano per forza un bug: spesso sono attesi nel percorso di test.
- User-space freestanding: non introdurre dipendenze implicite da libc. In `vfsd` usare helper locali
  (`vfsd_memcpy`, `vfsd_bzero`, ecc.), non assumere che `memcpy` venga risolta dal linker.

### Limiti noti della v1 M9-04

- I backend filesystem reali restano ancora bootstrap kernel-side dietro `SYS_VFS_BOOT_*`.
- `vfsd` non fa ancora garbage collection dello stato client/namespace dei PID terminati.
- La semantica `pivot_root()` e' sufficiente per bootstrap e test, ma non e' ancora il modello completo
  da container/runtime avanzato previsto piu' avanti in `M11-07`.

---

## Bug storici noti (risolti, non ripetere)

### Re-entrant schedule() race → PC=0 crash
`irq_restore()` prima di `sched_context_switch()` apre una finestra dove un timer IRQ
chiama `schedule()` rientrante, sovrascrivendo `next->sp` con un frame sullo stack di
`prev`. Al ripristino: x30=0 → `ret` salta a PC=0 → Instruction Abort.
**Risolto**: IRQ disabilitati durante il switch, poi `msr daifclr, #2` (non `irq_restore`).

### Direct priority mutation → double-insert in run queue
Impostare `task->priority = X` mentre il task è READY corrompe la run queue
(task finisce in due bucket simultaneamente). Vedere sezione scheduler sopra.
**Risolto**: usare sempre `sched_task_donate_priority()`.

### ksem holder starvation a PRIO_NORMAL
Holder a PRIO_NORMAL=128 starved dall'hog (stesso livello, FIFO precedente).
PI non aiuta: la donazione arriva solo DOPO che waiter entra in wait, ma holder
non riesce mai ad acquisire il lock perché hog gira in CPU-bound loop alla stessa prio.
**Risolto**: holder creato a PRIO_HIGH=32.
