# EnlilOS — Backlog 2: Userspace, Network, SMP
## Roadmap estesa dopo `BACKLOG.md` (con piu' milestone gia' implementate)

---

## Prerequisiti

Questo backlog assume completate tutte le milestone di `BACKLOG.md`:

| Completato | Descrizione |
|---|---|
| M1-M2 | Kernel foundation + interrupt RT + scheduler FPP |
| M3 | Syscall dispatcher + base + GPU + ANE |
| M4 | Tastiera + mouse + terminal + UTF-8 |
| M5 | VirtIO-blk + VFS + ext4 rw-full + initrd + recovery |
| M5b | GPU driver + display engine + 2D rendering |
| M6 | ELF loader statico/dinamico + execve |
| M7 | IPC sincrono RT + shell `nsh` (bootstrap minimale, sostituita da arksh in M8-08) |

---

## Principi RT — immutati, estesi al userspace

| Principio | Regola userspace |
|---|---|
| **No blocking syscall in hard-RT task** | I task hard-RT usano solo syscall con `SYSCALL_FLAG_RT` |
| **Priority donation transitiva** | IPC cross-server propaga la priorità del chiamante per tutta la catena |
| **Capability-based access** | Ogni risorsa (file, porta IPC, buffer GPU) è un token unforgeable |
| **Pre-fault obbligatorio** | Prima di entrare nel loop RT, il task pre-faulta stack + heap + shared mem |
| **WCET misurabile end-to-end** | Il profiler RT misura latenza dal submit syscall al completamento, incluso IPC |
| **Deadline propagation** | Un task che passa un job a un server trasmette anche la propria deadline |

---

## MILESTONE 8 — Process Model Completo

### ✅ M8-01 · fork() + Copy-on-Write MMU
**Priorità:** CRITICA (sblocca qualsiasi shell POSIX e toolchain)

**RT design:** `fork()` non è RT-safe — ma deve completare in tempo **bounded** per non
disturbare task a alta priorità in esecuzione.

- Page table L2/L3 copia parziale: solo le entry presenti (no sweep completo)
- Pagine parent marcate `COW` (R/O nei descrittori MMU entrambe le viste)
- Fault COW → `phys_alloc_page()` + copy + re-mark R/W → O(1) per pagina
- `fork()` = `clone_address_space()` + nuovo TCB + copia fd_table + copia `brk`
- Parent e figlio condividono le pagine fino al primo write; stack child è copia immediata
- Limite: `fork()` di un task hard-RT è **vietato** (panic se `current->flags & TASK_HARD_RT`)
- Implementato nel kernel: `sys_fork()`, clone `mm_space` con COW, fault handler write-on-COW,
  refcount PMM per pagina e resume del figlio dal frame syscall salvato
- Verifica dedicata: self-test `fork-cow` in QEMU e demo user-space `/FORKDEMO.ELF`

**Strutture:**
```c
/* Page table entry COW extension */
#define PTE_COW         (1UL << 55)   /* software bit disponibile su AArch64 */

/* mm_space: descrittore spazio di indirizzamento per processo */
typedef struct mm_space {
    uint64_t  *pgd;           /* Page Global Directory (L1) */
    uint32_t   refcount;      /* condiviso tra fork-parent/child */
    uintptr_t  brk;           /* program break corrente */
    uintptr_t  mmap_base;     /* base per mmap anonimo */
} mm_space_t;
```

**Dipende da:** M3-02 (`mmap`), M6-01 (page table layout stabilito da ELF loader)
**Sblocca:** M8-02 (mmap file), M8-03 (signal), toolchain POSIX completa

---

### ✅ M8-02 · mmap() File-Backed
**Priorità:** ALTA

**Stato attuale:** implementata v1 con VMA statiche kernel-side, syscall
`mmap/munmap/msync`, supporto `MAP_SHARED` / `MAP_PRIVATE`, demo `/MMAPDEMO.ELF`
e selftest `mmap-file` validato nel run completo `SUMMARY total=25 pass=25 fail=0`.

- `MAP_SHARED` / `MAP_PRIVATE` supportati su file descriptor VFS
- `MAP_ANONYMOUS` resta disponibile come base per heap e shared memory user-space
- `sys_mmap()` carica il contenuto file nelle pagine user e registra una `vm_area_t`
  per task in un pool statico bounded
- `MAP_PRIVATE` mantiene le modifiche locali al processo
- `MAP_SHARED` usa write-back esplicito su `msync()` e implicito su `munmap()`
- supporto operativo anche con fd remoti dietro `vfsd`, tramite shadow handle VFS
  locale usato dal kernel per `msync()` / `munmap()`
- separazione chiara tra `mmu.c` e `vmm.c`: mapping fisico/MMU da un lato, metadata
  VMA file-backed dall'altro

**Limiti v1 dichiarati:**
- mapping eager al `mmap()`, non ancora demand paging lazy guidato da page fault
- write-back a pagina intera su `msync()` / `munmap()`
- non RT-safe: resta pensato per loader, libc, task general-purpose e server

**Struttura file attuale:**
```
include/vmm.h         — metadata VMA file-backed
kernel/vmm.c          — pool statico VMA + msync/munmap write-back
kernel/syscall.c      — syscall mmap/munmap/msync
kernel/selftest.c     — selftest end-to-end `mmap-file`
user/mmap_demo.c      — demo EL0 `MAP_PRIVATE` + `MAP_SHARED`
```

**Dipende da:** M8-01 (MMU/COW stabile), M9-02 (fd remoti via `vfsd`)
**Sblocca:** tooling user-space piu' ricco, file I/O memory-mapped, porting libc futuro

---

### ✅ M8-03 · Signal Handling
**Priorità:** ALTA

Segnali POSIX minimali, compatibili con musl libc (M11-01).

- `sigaction(sig, act, oldact)` — installa handler utente per segnale
- `sigreturn()` — trampoline AArch64: kernel costruisce frame signal sullo stack utente,
  il handler esegue a EL0, poi `sigreturn` ripristina il contesto originale
- Segnali sincroni supportati: `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`
- Segnali asincroni supportati: `SIGINT`, `SIGTERM`, `SIGCHLD`, `SIGALRM`
- `SIGKILL` / `SIGSTOP`: non intercettabili, implementati direttamente nel kernel
- Maschera segnali: `sigmask` in `sched_tcb_t`, modificata da `sigprocmask()`
- `kill(pid, sig)`: syscall nr 134 — deposita segnale pendente nel TCB target
- Consegna segnale: al rientro da ogni eccezione/IRQ, prima di ERET → controlla `pending_signals`
- Implementato: `sigaction`, `sigprocmask`, `sigreturn`, `kill`, trampoline EL0 condiviso,
  consegna asincrona su return-to-user e routing `CTRL+C` console → `SIGINT`
- Implementato: `SIGCHLD` minimo su exit del figlio, demo `/SIGDEMO.ELF` e self-test `signal-core`

**RT constraint:** i task con `TASK_HARD_RT` possono bloccare tutti i segnali tranne
`SIGKILL`. Nessun handler utente viene invocato dentro il tick RT.

**Strutture:**
```c
typedef void (*sighandler_t)(int);

typedef struct {
    sighandler_t  sa_handler;
    uint64_t      sa_mask;       /* segnali bloccati durante handler */
    uint32_t      sa_flags;      /* SA_RESTART, SA_NODEFER, SA_RESETHAND */
} sigaction_t;

/* aggiunto a sched_tcb_t */
uint64_t   pending_signals;      /* bitmask segnali pendenti */
uint64_t   blocked_signals;      /* bitmask segnali bloccati */
sigaction_t signal_table[64];    /* handler per i 64 segnali standard */
```

---

### ✅ M8-04 · Process Groups, Sessions, Job Control
**Priorità:** MEDIA (richiesta da shell interattiva con `CTRL+Z`, `bg`, `fg`)

**Stato attuale:** implementata v1 con `pgid/sid` per-task, syscall EL0
`setpgid/getpgid/setsid/getsid/tcsetpgrp/tcgetpgrp`, `waitpid(WUNTRACED)`,
job-control signal path completo e demo `/JOBDEMO.ELF` validato dal selftest
`jobctl-core` nel run completo `SUMMARY total=25 pass=25 fail=0`.

- `setpgid()`, `getpgid()`, `setsid()`, `getsid()`
- `tcsetpgrp()` / `tcgetpgrp()` per controllo terminale (console)
- Segnali di job control: `SIGTSTP`, `SIGCONT`, `SIGTTIN`, `SIGTTOU`
- `waitpid(WUNTRACED)` per rilevare task stoppati
- Campi `pgid`, `sid` e `parent_pid` mantenuti nel contesto scheduler
- `CTRL+C` e `CTRL+Z` inviati al foreground process group invece che al singolo task
- `tty_read()` blocca i background job con `SIGTTIN`, `write()` su console con `SIGTTOU`
- la TTY di controllo viene adottata da sessioni interattive (`read` / `tcsetpgrp`)
  e non piu' da qualsiasi server che scrive su `stdout`

**Note v1 / limiti dichiarati:**
- `nsh` non espone ancora built-in `fg` / `bg`
- non esiste ancora distinzione completa tra controlling TTY multipli: il modello
  attuale copre una console globale coerente con il target QEMU di bootstrap

**Struttura file attuale:**
```
include/syscall.h      — ABI `setpgid/getpgid/setsid/getsid/tcsetpgrp/tcgetpgrp`
include/sched.h        — metadata scheduler per `parent_pid/pgid/sid`
include/tty.h          — foreground pgrp della console
include/signal.h       — delivery per process group e stato stop/report
kernel/sched.c         — ownership process group / sessione
kernel/syscall.c       — syscall job control + `waitpid(WUNTRACED)`
kernel/tty.c           — controllo terminale, `SIGTTIN/TTOU`, `CTRL+Z`
kernel/signal.c        — stop/continue/reporting verso il parent
kernel/selftest.c      — selftest end-to-end `jobctl-core`
user/job_demo.c        — demo EL0 `/JOBDEMO.ELF`
```

---

### ✅ M8-05 · mreact — Reactive Memory Subscriptions
**Priorità:** ALTA
**Originalità:** primitiva non presente in nessun OS esistente (Linux, XNU, Windows NT, seL4, Fuchsia, Plan 9)
**Stato attuale:** implementata v1 con backend cooperativo in kernel (`mreact_wait()` valuta il predicato e cede la CPU con `sched_yield()`), syscall EL0 `80–84`, demo user-space e selftest end-to-end.

**Problema che risolve:** eliminare l'intera classe dei polling loop su shared memory.
Oggi ogni OS costringe i task a scegliere tra:
- `futex(FUTEX_WAIT)` — solo condizione `== value` su un singolo `uint32_t`
- Hardware watchpoints — max 4, nessun predicato sul valore
- `inotify`/`kqueue` — solo filesystem, non RAM arbitraria
- Polling attivo con `nanosleep` — spreco CPU + latenza non deterministica

`mreact` permette: **"svegliami quando `*addr` soddisfa il predicato P"**, con latenza
kernel-guaranteed uguale al tempo tra la write e il ciclo IRQ successivo.

---

#### Syscall (range 80–89)

| Nr | Nome | RT-safe | Firma C |
|----|------|---------|---------|
| 80 | `mreact_subscribe` | No (setup) | `(void *addr, size_t size, mreact_pred_t pred, uint64_t value, uint32_t flags) → mreact_handle_t` |
| 81 | `mreact_wait` | **Sì** | `(mreact_handle_t h, uint64_t timeout_ns) → int` |
| 82 | `mreact_cancel` | Sì — O(1) | `(mreact_handle_t h) → int` |
| 83 | `mreact_subscribe_all` | No (setup) | `(mreact_sub_t *subs, uint32_t n, uint32_t flags) → mreact_handle_t` |
| 84 | `mreact_subscribe_any` | No (setup) | `(mreact_sub_t *subs, uint32_t n, uint32_t flags) → mreact_handle_t` |

**Predicati supportati (`mreact_pred_t`):**

| Costante | Condizione |
|---|---|
| `MREACT_EQ` | `*addr == value` |
| `MREACT_NEQ` | `*addr != value` |
| `MREACT_GT` | `*addr > value` |
| `MREACT_LT` | `*addr < value` |
| `MREACT_BITMASK_SET` | `(*addr & value) == value` |
| `MREACT_BITMASK_CLEAR` | `(*addr & value) == 0` |
| `MREACT_CHANGED` | qualsiasi write (indipendente dal valore) |
| `MREACT_RANGE_IN` | `lo <= *addr <= hi` (value = `lo | (hi << 32)`) |
| `MREACT_RANGE_OUT` | `*addr < lo` o `*addr > hi` |

**Flag:**
- `MREACT_ONE_SHOT` — si disattiva al primo trigger
- `MREACT_PERSISTENT` — resta attivo fino a `mreact_cancel`
- `MREACT_EDGE` — trigger solo al passaggio falso→vero (non se già vero al subscribe)
- `MREACT_LEVEL` — trigger immediato se la condizione è già vera al subscribe
- `MREACT_SAMPLE(N)` — campiona ogni N write (ottimizzazione per indirizzi ad alta frequenza)

---

#### Implementazione kernel

**Backend v1 implementato:**

1. `mreact_subscribe()` registra una o piu' subscription nel pool statico kernel
2. `mreact_wait()` blocca il task chiamante con timeout bounded
3. `mreact_wait()` valuta i predicati nel kernel sul relativo `mm_space`
4. se la condizione non e' ancora vera, la syscall cede la CPU con `sched_yield()` e riprova
5. `ONE_SHOT`, `PERSISTENT`, `LEVEL`, `EDGE`, `subscribe_all()` e `subscribe_any()` sono supportati

**Profilo di latenza v1:**
- nessun polling in user-space
- nessuna evaluation nel contesto hard-IRQ
- costo kernel bounded dal numero massimo di handle/subscription del pool statico
- latenza legata al prossimo slice/switch di scheduler, non ancora al solo tick IRQ

**Ottimizzazione rinviata:**
Il design originario con `write-protect + Data Abort` e `MREACT_SAMPLE(N)` via PMU/watchpoint
resta valido come evoluzione successiva del backend, ma non e' necessario per usare gia' l'API
EL0 e il modello di programmazione di `mreact`.

---

#### Casi d'uso

**1. Task di controllo RT su sensore shared memory (caso principale):**
```c
// Sensore temperatura in shared memory — scritto da driver a 1kHz
volatile int32_t *temp = shm_sensor;

// Registra subscription una volta al setup
mreact_handle_t h = mreact_subscribe(
    temp, sizeof(int32_t), MREACT_GT, 80, MREACT_PERSISTENT
);

// Loop RT — zero polling, zero CPU sprecata
while (1) {
    int r = mreact_wait(h, DEADLINE_NS);
    if (r == 0) activate_cooling();
    else        handle_missed_deadline();
}
```

**2. IPC senza ring buffer — zero-copy observer:**
```c
// Produttore scrive direttamente in shared memory
*shared_result = compute();

// Consumatore svegliato dal kernel alla write, senza ring buffer intermedio
mreact_handle_t h = mreact_subscribe(shared_result, 4,
                                      MREACT_CHANGED, 0, MREACT_PERSISTENT);
mreact_wait(h, timeout);
```

**3. Invariant enforcement hardware-guaranteed:**
```c
// "Se safety_flag diventa 0, il kernel lo sa prima del tick successivo"
mreact_handle_t h = mreact_subscribe(
    &safety_flag, sizeof(uint32_t), MREACT_EQ, 0, MREACT_PERSISTENT
);
// Task watchdog a priorità massima — non polling, zero latenza aggiuntiva
```

**4. Consensus RT tra N task:**
```c
// Svegliami solo quando TUTTI e 3 i sensori hanno segnalato pronto
mreact_sub_t subs[3] = {
    { sensor_a, 4, MREACT_EQ, READY, MREACT_EDGE },
    { sensor_b, 4, MREACT_EQ, READY, MREACT_EDGE },
    { sensor_c, 4, MREACT_EQ, READY, MREACT_EDGE },
};
mreact_handle_t h = mreact_subscribe_all(subs, 3, MREACT_ONE_SHOT);
mreact_wait(h, deadline);
// Eseguito esattamente una volta, quando tutti e 3 sono pronti
```

---

**Struttura file:**
```
kernel/mreact.c         — implementazione v1: pool statico, predicati, wait/cancel
include/mreact.h        — strutture pubbliche, predicati, flag
kernel/syscall.c        — syscall 80–84 + integrazione con mmap/exec/cleanup task
kernel/mmu.c            — mmap anonimo EL0 nel window user
kernel/selftest.c       — selftest end-to-end `mreact-core`
user/mreact_demo.c      — demo EL0 che attende su word mappata con `mmap()`
```

**Dipende da:** M3-01 (syscall dispatcher), M2-03 (sched_block/unblock), M6-01 (user-space + MMU task-private)
**Potenziato da:** M7-01 (IPC RT — mreact come alternativa a IPC per shared-memory pattern), M9-01 (capability — la subscription diventa una capability revocabile)

---

### ✅ M8-06 · Semafori Kernel Nativi (`ksem`)
**Priorità:** ALTA

**Stato attuale:** implementata v1 con pool statici (`ksem`, waiter, handle ref),
syscall EL0 `85–94`, named semaphore + anon, `post/wait/timedwait/trywait/getvalue`,
cleanup su `exit/exec/signal`, PI best-effort riusando il donation slot dello scheduler
e selftest end-to-end `ksem-core`. Il wait contended e' bounded/cooperativo
(`sched_yield()` + deadline su `timer_now_ms()`), quindi il contratto RT e' gia'
usabile ma il timer-wheel dedicato resta un margine di miglioramento futuro.

> **Distinzione architetturale:** EnlilOS prevede due livelli di semafori.
> **Livello 1 — `ksem` (questa milestone):** primitiva kernel nativa con syscall proprie,
> pool statico, RT-safe, priority inheritance integrata. Non richiede musl libc.
> **Livello 2 — `sem_t` POSIX:** wrapper musl sopra `ksem`, esposto via `<semaphore.h>`.
> Implementato in `M11-02d`, quando il layer `pthread`/POSIX viene chiuso lato musl.

I semafori Mach (-36/-37) previsti in M11-04d come trap stub vengono **rimossi** da
quel layer e mappati direttamente su `ksem` — coerenza garantita su tutti i path.

---

#### Perché non basta il futex (M11-02)

| | `futex` | `ksem` |
|---|---|---|
| Semantica | solo `== value` su indirizzo user | contatore con P/V semantica Dijkstra |
| Naming | anonimo (indirizzo) | named (`/sem/nome`) + anonimo |
| Cross-process | solo via shared memory | kernel-managed, basta l'handle |
| Priority inheritance | manuale (`FUTEX_LOCK_PI`) | integrata in ogni `ksem_wait` |
| Bounded wait | no (solo timeout) | `ksem_timedwait` con deadline RT-safe |
| Contatore | no | sì — `ksem_getvalue` in O(1) |

---

#### Syscall (range 85–94, adiacente a mreact 80–84)

| Nr | Nome | RT-safe | Firma C |
|----|------|---------|---------|
| 85 | `ksem_create` | No | `(const char *name, uint32_t value, uint32_t flags) → ksem_t` |
| 86 | `ksem_open` | No | `(const char *name, uint32_t flags) → ksem_t` |
| 87 | `ksem_close` | Sì — O(1) | `(ksem_t s) → int` |
| 88 | `ksem_unlink` | No | `(const char *name) → int` |
| 89 | `ksem_post` | **Sì** | `(ksem_t s) → int` |
| 90 | `ksem_wait` | **Sì** | `(ksem_t s) → int` |
| 91 | `ksem_timedwait` | **Sì** | `(ksem_t s, uint64_t timeout_ns) → int` |
| 92 | `ksem_trywait` | **Sì** — O(1) | `(ksem_t s) → int` — EAGAIN se zero |
| 93 | `ksem_getvalue` | **Sì** — O(1) | `(ksem_t s, int32_t *val) → int` |
| 94 | `ksem_anon` | No | `(uint32_t value) → ksem_t` — semaforo anonimo (inter-thread) |

**Flag `ksem_create`:**
- `KSEM_PRIVATE` — anonimo (equivalente a `sem_init` POSIX)
- `KSEM_SHARED` — condiviso tra processi con lo stesso nome
- `KSEM_RT` — abilita priority inheritance (descritto sotto)
- `KSEM_ONESHOT` — si autodistrugge dopo il primo post/wait

---

#### Strutture

```c
/* Handle opaco (indice nel pool statico) */
typedef uint32_t ksem_t;   /* 0 = KSEM_INVALID */

/* Entry interna kernel (pool statico, nessun kmalloc) */
typedef struct ksem_entry {
    char          name[32];      /* vuoto se anonimo */
    atomic_int    value;         /* contatore semaforo — accesso atomico LDAXR/STLXR */
    uint32_t      flags;
    uint32_t      refcount;      /* processi che lo hanno aperto */
    /* Coda dei waiter (wait queue intrusive, pool separato) */
    sched_tcb_t  *wait_head;     /* testa lista FIFO */
    sched_tcb_t  *wait_tail;
} ksem_entry_t;

#define KSEM_MAX  128
static ksem_entry_t ksem_pool[KSEM_MAX];   /* pool statico */
```

---

#### Implementazione

**`ksem_post` (V — signal):**
1. `LDAXR/STLXR`: incrementa atomicamente `value`
2. Se `wait_head != NULL` (c'è almeno un waiter): non incrementa, sblocca il waiter
   con `sched_unblock()` trasferendo il token direttamente — no spurious wakeup
3. WCET: O(1) — due istruzioni atomiche + un `sched_unblock`

**`ksem_wait` (P — proberen):**
1. `LDAXR`: legge `value`
2. Se `> 0`: decrementa con `STLXR`, ritorna 0 — O(1) nel caso non-contended
3. Se `== 0`: inserisce il TCB corrente in `wait_tail`, chiama `sched_block()` → blocca
4. Al risveglio da `ksem_post`: ritorna 0 (il token è già stato trasferito)
5. WCET: O(1) nel caso non-contended (fast path — nessun cambio di contesto)

**Priority Inheritance con `KSEM_RT`:**
- Quando un task ad alta priorità P1 si blocca su `ksem_wait`, il kernel identifica
  il task P2 che ha l'ultima "intenzione di post" tracciata nel semaforo
- P2 riceve temporaneamente la priorità di P1 (`sched_donate_priority`) per la durata
  dell'attesa — esattamente come per i mutex RT (M7-01)
- Al `ksem_post` di P2: la priorità donata viene restituita, P1 viene sbloccato
- Questo richiede un campo `owner_tcb` nel `ksem_entry_t` (valido solo per semafori
  binari usati come mutex; per semafori contatori la PI è best-effort)

**`ksem_timedwait` RT-safe:**
- Inserisce il task in `wait_tail` + installa un timer one-shot (timer wheel M2-02)
- Se il timer scatta prima del post: rimuove il task dalla wait queue, ritorna `ETIMEDOUT`
- WCET: O(1) + overhead inserimento timer wheel = O(1)

**Named semaphore (kernel namespace `/sem/`):**
- Hash table di 64 entry sul nome (FNV-1a, collisioni in pool)
- `ksem_create` con nome già esistente + `O_EXCL` → `EEXIST`
- `ksem_unlink`: rimuove dal namespace; semaforo continua a vivere finché refcount > 0

**Integrazione con POSIX (M11-01):**
```c
/* In musl arch/aarch64/src/semaphore/ — sostituisce sem_open.c */
sem_t *sem_open(const char *name, int oflag, ...) {
    ksem_t h = ksem_open(name, oflag);   /* syscall nr 86 */
    /* wrappa handle in sem_t per compatibilità POSIX */
    ...
}
int sem_post(sem_t *s)         { return ksem_post(s->_handle); }
int sem_wait(sem_t *s)         { return ksem_wait(s->_handle); }
int sem_timedwait(sem_t *s, …) { return ksem_timedwait(s->_handle, ts_to_ns(ts)); }
int sem_getvalue(sem_t *s, int *v) { return ksem_getvalue(s->_handle, v); }
```

**Struttura file:**
```
kernel/ksem.c       — pool, create/open/close/unlink, post/wait/timedwait/trywait
include/ksem.h      — strutture pubbliche, costanti
kernel/syscall.c    — ksem_init() + syscall 85-94 al boot
```

**Dipende da:** M3-01 (syscall dispatcher), M2-03 (sched_block/unblock), M2-02 (timer per timedwait)
**Sblocca:** M11-02 (`sem_t` POSIX sopra `ksem`), M8-07 (monitor usa ksem come lock interno opzionale)

---

### ✅ M8-07 · Monitor Kernel (`kmon`)
**Priorità:** ALTA (dipende da M8-06)

**Stato:** implementazione stabile. La race condition nel selftest `kmon-core` è stata
corretta: il task holder ora usa `sched_block()` invece del polling `sched_yield()`,
e il signaler chiama `sched_unblock(holder)` esplicitamente. La priority-inheritance
è verificabile abbassando la priorità del holder a `PRIO_LOW` prima del block.
Il monitor è ora usato in produzione in `kernel/ext4.c` (`g_ext4_open_lock`) per
serializzare accessi concorrenti a `ext4_open_vfs`.

**Profilo implementato:**
- monitor kernel con handle opaco `kmon_t`
- mutex implicito con owner tracking, recursion check e priority ceiling / donation best-effort
- fino a 8 condition queue per monitor
- `wait`, `signal`, `broadcast`, `timedwait`
- cleanup automatico dei waiter su `exit`, `execve` e terminazione task
- integrazione completa nel dispatcher syscall
- lock kmon in `ext4_open_vfs` — primo uso produzione nel kernel

**Semantica:**
- `enter/exit` fanno lock/unlock del monitor in kernel
- `wait(cond)` rilascia il lock e parcheggia il task sulla condition
- `signal(cond)` e `broadcast(cond)` spostano i waiter dalla condition alla `enter_q`
- il lock torna al prossimo waiter su `kmon_exit()`
- profilo **Mesa-style / signal-and-continue**
- il flag `KMON_HOARE` resta esposto come ABI, ma l'handoff Hoare stretto è rinviato a una revisione successiva

**Syscall effettive registrate:**

| Nr | Nome | RT-safe | Firma C |
|----|------|---------|---------|
| 95 | `kmon_create` | No | `(uint32_t prio_ceiling, uint32_t flags) -> kmon_t` |
| 96 | `kmon_destroy` | No | `(kmon_t m) -> int` |
| 97 | `kmon_enter` | Sì | `(kmon_t m) -> int` |
| 98 | `kmon_exit` | Sì | `(kmon_t m) -> int` |
| 99 | `kmon_wait` | Sì | `(kmon_t m, uint8_t cond, uint64_t timeout_ns) -> int` |
| 110 | `kmon_signal` | Sì | `(kmon_t m, uint8_t cond) -> int` |
| 111 | `kmon_broadcast` | Sì | `(kmon_t m, uint8_t cond) -> int` |

**Nota ABI:** `100-109` restano riservate all'ANE, quindi `signal` e `broadcast`
sono state collocate a `110-111`.

**Copertura test (`selftest kmon-core`):**
- contesa su `enter()` con donation/ceiling (holder a PRIO_LOW, waiter a PRIO_HIGH)
- holder bloccato con `sched_block()`, sbloccato con `sched_unblock()` — nessun polling
- `wait/signal` su condition queue
- `timedwait` con ritorno `-ETIMEDOUT`
- distruzione del monitor dopo drain dei waiter

**Struttura file:**
```
include/kmon.h       — API pubblica, handle, flag
kernel/kmon.c        — pool monitor/waiter, enter/exit, wait/signal/broadcast
kernel/syscall.c     — wrapper syscall + init al boot
include/syscall.h    — numeri syscall 95-99 / 110-111
kernel/selftest.c    — selftest kmon-core
kernel/ext4.c        — g_ext4_open_lock: primo uso produzione
```

**Dipende da:** M3-01, M2-03, M8-06
**Sblocca:** M11-02 (pthread condvar/mutex lato libc), server user-space che vogliono
serializzare richieste con PI kernel-side

---

### ✅ M8-07b · Infrastruttura Syscall e User-Space (complemento)
**Priorità:** INFRASTRUTTURA — non è una milestone utente ma corregge e consolida

Correzioni e aggiunte apportate contestualmente alla stabilizzazione di M8-07:

**`SYS_YIELD = 20`** — nuova syscall `sched_yield()` da EL0
- Path: `sys_call0(SYS_YIELD)` invoca `sched_yield()` nel kernel
- Sostituisce il pattern `asm volatile("" ::: "memory")` nei demo user-space
- Necessario per signal_demo, fork_demo e qualsiasi loop di attesa cooperativo in EL0

**Exit code propagation** — `sys_exit(code)` porta il codice al TCB
- `sched_task_exit_with_code(int32_t code)` salva il codice nel `sched_task_ctx_t`
- `sched_task_get_exit_code()` permette ai selftest di verificarlo
- Ogni demo EL0 può ora exit con codice diverso da 0 per segnalare fallimento

**`include/user_svc.h`** — header condiviso per wrapper SVC da EL0
- `user_svc0..6()` con clobber list completa (x9-x18, cc)
- `user_svc_exit(code, nr)` — `__attribute__((noreturn))` + loop `wfe`
- Elimina il codice duplicato in ogni demo; tutti i file user-space lo includono

**Correzione `SYS_KILL = 134`** (era 129)
- 129 era riservato (conflitto con range Linux che non usiamo, ma ambiguo)
- 134 è stabile nell'ABI EnlilOS; il backlog M8-03 aggiornato di conseguenza

**Retry su `vfsd_proxy_call()`**
- Loop con deadline 1s su EBUSY/EAGAIN — tollera la finestra di avvio di vfsd
- Necessario per i selftest che avviano vfsd e subito dopo fanno richieste VFS

**Numeri syscall reali M8-08a/b/c**
- `chdir` = 28
- `getcwd` = 29
- `pipe` = 34
- `dup` = 35
- `dup2` = 36
- `tcgetattr` = 37
- `tcsetattr` = 38
- `isatty` = 41

---

### 🟨 M8-08 · Porting arksh — Shell di Default EnlilOS
**Priorità:** ALTA
**Repo:** https://github.com/nicolorisitano82/arksh
**Licenza:** MIT — compatibile con EnlilOS, nessun vincolo di distribuzione

> arksh sostituisce `nsh` (M7-02) come shell interattiva di default.
> `nsh` rimane disponibile come shell di recovery (initrd, modalità single-user)
> perché non ha dipendenze esterne. arksh è la shell normale dell'utente.

**Stato attuale:** `M8-08a/b/c/d/e/f` completate `v1`. Il bootstrap shell-side e'
chiuso: toolchain, smoke CMake, login shell bridge `/bin/arksh`, layout home/config,
fallback `/bin/nsh` e binario reale esterno `/usr/bin/arksh.real` sono reali. Restano
aperti i pezzi post-bootstrap piu' avanzati (plugin `.so`, UX/history avanzata e
hardening del port hosted).

**Perché arksh invece di bash/dash:**
- Zero dipendenze esterne a runtime — solo libc (musl M11-01)
- C11 puro, già multi-platform (Linux/macOS/Windows) → porting AArch64 diretto
- Modello a oggetti + pipeline `|>` + tipi ricchi → shell moderna per un OS moderno
- Codebase compatta, documentazione in italiano, plugin system stabile

---

#### Analisi dipendenze POSIX da rimappare

arksh usa solo la standard C library + API OS. Su EnlilOS tutte le API
necessarie sono già pianificate nelle milestone precedenti o in questo backlog:

| API POSIX usata da arksh | Disponibile in EnlilOS | Milestone |
|---|---|---|
| `fork()` / `execve()` | Sì | M8-01 + M6-02 |
| `waitpid()` | Sì | M3-02 |
| `pipe()` | Sì (v1) | M8-08a |
| `dup2()` | Sì (v1) | M8-08a |
| `open/read/write/close` | Sì | M3-02 + M5-02 |
| `getcwd()` / `chdir()` | Sì (v1) | M8-08b |
| `stat()` / `access()` | Sì | M3-02 (fstat) + M5-02 |
| `opendir()` / `readdir()` | Sì | M5-02 |
| `signal()` / `sigaction()` | Sì | M8-03 |
| `tcgetattr()` / `tcsetattr()` | Sì (subset v1) | M8-08c |
| `isatty()` | Sì (v1) | M8-08c |
| `getenv()` / `setenv()` | Parziale | M8-08b |
| `dlopen()` / `dlsym()` | Sì (v1) | M11-03 |
| `realpath()` | Parziale lato libc | M8-08b |
| `glob()` | Vedi M8-08d | questa milestone |
| `fnmatch()` | Vedi M8-08d | questa milestone |
| XDG dirs (`~/.config/arksh`) | Vedi M8-08b | questa milestone |

---

#### ✅ M8-08a · Syscall pipe() e dup2()

Necessarie per la pipeline tra comandi (`cmd1 | cmd2`).

**Stato attuale:** implementata v1 con `pipe/dup/dup2` kernel-side, file descriptor
condivisi a refcount corretto su `fork()`/`dup*()`, demo `/POSIXDEMO.ELF` e selftest
`posix-ux` validato nel run completo `SUMMARY total=25 pass=25 fail=0`.

**`pipe(int fd[2])` — syscall nr 34:**
- Crea una coppia di fd: `fd[0]` lettura, `fd[1]` scrittura
- Implementazione kernel: buffer circolare statico da 4096 byte per pipe
- Pool di `MAX_PIPES = 32` pipe kernel (statico, no kmalloc)
- Scrittura blocca cooperativamente se piena; lettura blocca cooperativamente se vuota
- `close(fd[1])` → EOF sul lettore quando il conteggio writer scende a zero
- `O_NONBLOCK` supportato sul path pipe con `-EAGAIN` se il buffer è pieno/vuoto
- `isatty(fd_pipe)` ritorna `0`, `fstat(fd_pipe)` espone `S_IFIFO`

```c
typedef struct {
    uint8_t   buf[4096];
    uint32_t  read_pos, write_pos;
    uint32_t  size;
    uint32_t  readers;
    uint32_t  writers;
} pipe_t;

static pipe_t pipe_pool[MAX_PIPES];
```

**`dup2(int oldfd, int newfd)` — syscall nr 36:**
- Condivide lo stesso `fd_object_t` del `oldfd` con refcount incrementato
- Se `newfd` è già aperto: chiude prima il vecchio oggetto
- Corretto anche attraverso `fork()`: la table child clona gli stessi oggetti condivisi

**`dup(int oldfd)` — syscall nr 35:**
- Trova il primo slot libero in `fd_table[pid]` e ci copia `oldfd`
- `close()` rilascia solo il riferimento del singolo fd; l'oggetto sottostante si chiude all'ultimo ref

---

#### ✅ M8-08b · Environment e CWD

Necessari per `cd`, variabili `$PATH`, `$HOME`, `$PWD`, ecc.

**Stato attuale:** implementata v1. `getcwd/chdir` sono disponibili per i task EL0,
il `cwd` è risolto tramite `vfsd` e i task lanciati con `elf64_spawn_path()` ricevono
un ambiente bootstrap (`PATH`, `HOME`, `PWD`, `TERM`, `USER`). Demo e validazione
runtime passano tramite `/POSIXDEMO.ELF` e selftest `posix-ux`.

**Environment (`getenv`/`setenv`/`unsetenv`/`environ`):**
- Array `envp[]` già passato nell'ABI stack al `execve` (M6-01 auxv) — `nsh`/demo lo leggono
- `elf64_spawn_path()` fornisce env bootstrap di default: `PATH=/:/dev:/data:/sysroot`,
  `HOME=/`, `PWD=/`, `TERM=enlilos`, `USER=root`
- `execve()` continua a usare l'`envp` esplicitamente fornito dal caller
- `setenv`/`unsetenv`: restano responsabilità della libc user-space (M11-01), non del kernel

**`getcwd(char *buf, size_t size)` — syscall nr 29:**
- Il kernel proxya la richiesta verso `vfsd`, che è la source of truth del namespace mount + `cwd`
- `getcwd` copia il path risolto in user-space

**`chdir(const char *path)` — syscall nr 28:**
- Risolve il path via `vfsd`, aggiorna il `cwd` privato del task e rispetta i namespace mount
- `chdir` di un path relativo usa la vista privata del processo, coerente con `unshare(CLONE_NEWNS)`
- Ritorna `ENOENT` se il path non esiste, `ENOTDIR` se non è una directory

**`realpath(const char *path, char *resolved)` — implementato in musl** sopra
`getcwd` + operazioni string — nessuna syscall aggiuntiva.

---

#### ✅ M8-08c · Terminal Control (termios)

Necessario per il REPL interattivo di arksh: syntax highlighting, autosuggestion,
history con frecce, Ctrl+C/D/Z.

**Stato attuale:** implementata v1 con subset `termios`, `isatty()`, modalità
canonical/raw sulla console globale e integrazione con la line discipline esistente.
La copertura runtime passa da `/POSIXDEMO.ELF` e selftest `posix-ux`.

**`tcgetattr(int fd, struct termios *t)` — syscall nr 37:**
**`tcsetattr(int fd, int action, const struct termios *t)` — syscall nr 38:**

Il kernel mantiene uno `struct termios` per la console attiva:

```c
/* Sottoinsieme minimale per arksh */
typedef struct {
    uint32_t  c_iflag;   /* ICRNL, IXON */
    uint32_t  c_oflag;   /* OPOST, ONLCR */
    uint32_t  c_cflag;   /* CS8, CREAD */
    uint32_t  c_lflag;   /* ECHO, ECHOE, ICANON, ISIG, IEXTEN */
    uint8_t   c_cc[20];  /* VMIN, VTIME, VINTR, VEOF, VERASE, VKILL... */
} termios_t;
```

- Modalità **canonical** (default): line buffering, Backspace, Ctrl+C → SIGINT
- `Ctrl+Z` rispetta `ISIG` e genera `SIGTSTP`
- Modalità **raw** (`~ICANON`): arksh la abilita per il REPL — ogni byte arriva subito,
  senza buffering di riga; cursor movement con sequenze ANSI
- `isatty(fd)` — syscall nr 41: ritorna 1 se `fd` è connesso alla console, 0 altrimenti
- `tcsetattr(TCSANOW)` è supportato; `TCSADRAIN` / `TCSAFLUSH` al momento convergono sullo stesso path

**Limiti v1 dichiarati:**
- `VMIN/VTIME` non hanno ancora una semantica completa POSIX
- lo stato `termios` è globale sulla console, non ancora per-open-file o per-pty
- il supporto REPL avanzato di arksh richiederà ancora il build system dedicato e
  l'integrazione shell di `M8-08e/M8-08f`; `glob/fnmatch` bootstrap sono già disponibili

**Sequenze ANSI necessarie per il REPL arksh:**
- `\e[A`/`\e[B` — su/giù (history navigation)
- `\e[C`/`\e[D` — destra/sinistra (cursor movement)
- `\e[2K\r` — cancella riga corrente (redraw prompt)
- `\e[?25h`/`\e[?25l` — mostra/nasconde cursore
- `\e[3m`, `\e[0m`, `\e[1m`, `\e[3Xm` — colori per syntax highlighting

Il driver UART PL011 (esistente) + la line discipline M4-03 già gestisce le sequenze
di input; le sequenze ANSI di output sono semplice `write()` verso fd=1.

---

#### M8-08d · glob() e fnmatch()

Necessari per l'espansione wildcard (`*.c`, `src/**/*.h`).

**Stato attuale:** completata `v1`

Implementati interamente in **musl user-space** sopra `opendir`/`readdir` già disponibili:
- `glob(pattern, flags, errfunc, pglob)` — espande wildcard chiamando `readdir` sul VFS
- `fnmatch(pattern, string, flags)` — matching puro su stringhe, zero syscall
- `FNM_PATHNAME`, `FNM_NOESCAPE`, `FNM_PERIOD` — flag standard supportati
- Nessuna syscall aggiuntiva: si costruisce sopra M5-02 VFS `readdir`

**Deliverable chiusi nella v1:**
- header bootstrap `dirent.h`, `fnmatch.h`, `glob.h` nel sysroot musl minimale
- `readdir()` user-space sopra `SYS_GETDENTS`, senza introdurre nuove syscall per wildcard
- supporto `GLOB_MARK` e `GLOB_APPEND` oltre al subset minimo richiesto
- smoke `/MUSLGLOB.ELF`, comando boot `muslglob`, selftest `musl-glob`

**Nota tecnica importante:**
- il `malloc()` bootstrap usa `mmap()` per ogni allocazione; `glob()` quindi non può
  permettersi un modello "una allocazione per match". La v1 usa uno store compatto unico
  per `gl_pathv` + string pool, così evita `GLOB_NOSPACE` e frammentazione artificiale

---

#### ✅ M8-08e · Build System e Toolchain

**Stato attuale:** completata `v1`

**Deliverable chiusi nella v1:**
- toolchain file CMake reale in `tools/enlilos-aarch64.cmake`
- target host-side `make arksh-configure` e `make arksh-build`
- smoke host-side `make arksh-smoke`
- compat layer di riferimento in `compat/arksh/`
- smoke ELF `/ARKSHSMK.ELF`, comando boot `arkshsmoke`, selftest `arksh-toolchain`

**Build integration reale:**
```makefile
ARKSH_DIR            ?= $(CURDIR)/arksh
ARKSH_BUILD_DIR      ?= toolchain/build/arksh
ARKSH_TOOLCHAIN_FILE  = tools/enlilos-aarch64.cmake

make arksh-configure ARKSH_DIR=/percorso/arksh
make arksh-build     ARKSH_DIR=/percorso/arksh
make arksh-smoke
```

**Toolchain file CMake (`v1`):**
```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "${ENLILOS_ROOT}/toolchain/bin/aarch64-enlilos-musl-gcc")
set(CMAKE_SYSROOT    "${ENLILOS_ROOT}/toolchain/sysroot")
set(CMAKE_C_FLAGS_INIT "-DENLILOS=1")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
```

**Compatibilità arksh `v1`:**
- il repo `arksh` non e' vendorizzato in-tree: il sorgente va passato via `ARKSH_DIR`
- `compat/arksh/enlilos.c` documenta lo shim minimo da contribuire/adattare upstream
- `compat/arksh/README.md` spiega il workflow supportato oggi
- `make arksh-build` builda esplicitamente il target `arksh` del progetto esterno,
  evitando i target benchmark/test che richiedono ancora API hosted fuori perimetro
- quando `toolchain/build/arksh/arksh` esiste, l'`initrd` lo impacchetta come
  `/usr/bin/arksh.real` senza richiedere step manuali aggiuntivi

**Smoke CMake/toolchain:**
- `toolchain/cmake-smoke/` contiene un progetto CMake minimale cross-buildato con la toolchain EnlilOS
- lo smoke verifica:
  - define `ENLILOS=1`
  - link statico via wrapper `aarch64-enlilos-musl-gcc`
  - header `termios`
  - `pipe/dup2`
  - `glob()` su VFS
- runtime output: `/data/ARKSHSMK.TXT`

**Nota tecnica importante chiusa in questa milestone:**
- i target CMake esterni **non possono ereditare** i `CFLAGS/LDFLAGS` del kernel.
  Se il configure gira sotto `make` senza pulire l'ambiente, CMake assorbe
  `-T linker.ld -nostdlib` e il link del progetto esterno fallisce. La v1
  risolve il problema con `env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS`
  nei target `arksh-configure` e `arksh-smoke`.

---

#### ✅ M8-08f · Integrazione nel Sistema

**Stato attuale:** completata `v1`

**Deliverable chiusi nella v1:**
- `initrd` contiene:
  - `/INIT.ELF` -> launcher login shell
  - `/ARKSHBOOT.ELF` -> selftest ELF non interattivo
  - `/bin/arksh` -> launcher statico di bootstrap
  - `/usr/bin/arksh.real` -> shell reale esterna quando il build host-side e' disponibile
  - `/bin/nsh` -> recovery shell esplicita
  - `/etc/arkshrc`
  - `/home/user/.config/arksh/arkshrc`
- `kernel/main.c` auto-lancia la login shell di default dopo `bootcli_init()`
- comando boot `arksh` per rilanciare la login shell dal monitor
- `nsh` resta richiamabile esplicitamente come fallback/recovery
- directory plugin predisposta: `/usr/lib/arksh/plugins/`

**Login/home bootstrap reale:**
- al boot il kernel prepara su `/data` il layout persistente:
  `/data/home/user/.config/arksh/arkshrc`
  `/data/home/user/.local/state/arksh/history`
- il launcher `arksh` fa bind mount `/data/home -> /home` e prova `chdir("/home/user")`
- environment bootstrap riallineato a:
  `PATH=/bin:/usr/bin`, `HOME=/home/user`, `PWD=/`,
  `SHELL=/bin/arksh`, `TERM=vt100`, `USER=user`

**Fallback e limiti onesti della v1:**
- se `/usr/bin/arksh` o `/usr/bin/arksh.real` non sono presenti, il launcher degrada
  automaticamente su `/bin/nsh`
- la history persistente e' gia' predisposta e validata kernel-side su
  `/data/home/user/.local/state/arksh/history`
- l'accesso EL0 interattivo allo stesso path tramite `/home/...` non e' ancora usato
  come criterio di successo del selftest, perche' oggi quel path resta timing-sensitive;
  il consumo completo lato shell reale viene rimandato alla fase post-bootstrap

**Validazione chiusa:**
- comando boot `arksh`
- selftest `arksh-login`
- build host-side reale: `make arksh-build ARKSH_DIR=...`
- packaging verificato: `boot/initrd.cpio` contiene `usr/bin/arksh.real`
- suite runtime piu' recente: `SUMMARY total=44 pass=44 fail=0`

**Plugin system (dopo `M11-03`):**
- `dlopen`/`dlsym` disponibili dopo il dynamic linker
- plugin arksh compilati come `.so` AArch64 con la toolchain musl
- directory plugin gia' predisposta: `/usr/lib/arksh/plugins/`

---

#### M8-08g · Layout Tastiera Multipli (`us`, `it`)

- **Stato attuale:** completata `v1`

Necessario per rendere la console e la shell usabili in scenari reali non-US.
La roadmap deve prevedere almeno layout **US** e **Italiano** (`it`) fin dalla
prima iterazione utile.

- Separare definitivamente `scancode/keycode` dal carattere finale:
  `device event -> keycode -> keysym -> Unicode UTF-8`
- Tabelle layout statiche O(1) nel hot path, senza allocazioni:
  `us` e `it` come baseline iniziale
- Un layout attivo per console/TTY/sessione shell, ereditato dai processi figli
- Supporto minimo richiesto per `it`:
  lettere accentate (`à`, `è`, `é`, `ì`, `ò`, `ù`), simboli italiani più usati,
  `Shift`, `Ctrl`, `AltGr` e tasti morti basilari
- API compatibile con il sottosistema input esistente:
  `keyboard_getc()` resta disponibile come path legacy,
  ma il layer nuovo espone anche eventi con `keycode + modifiers + unicode`
- Stessa infrastruttura per tutte le origini input:
  PS/2 legacy, VirtIO input, USB HID (M16-03)
- Cambio layout a runtime tramite comando shell tipo `loadkeys it`
  e query via `kbdlayout`
- Persistenza configurazione in `/etc/vconsole.conf` o file equivalente:
  `KEYMAP=it`
- Repository layout iniziale:
  `/usr/share/kbd/keymaps/us.map`
  `/usr/share/kbd/keymaps/it.map`

**Chiusura v1:**
- pipeline reale `keycode -> keysym -> Unicode UTF-8` nel driver tastiera
- layout integrati `us` e `it` con `Shift`, `Ctrl`, `Alt`, `AltGr` e dead keys basilari
- API nuova con eventi (`keycode/modifiers/keysym/unicode`) e compat legacy `keyboard_getc()`
- utility utente bootstrap `loadkeys` e `kbdlayout`
- persistenza layout via `/data/etc/vconsole.conf` con fallback `/etc/vconsole.conf`
- keymap bootstrap presenti in `/usr/share/kbd/keymaps/us.map` e `it.map`
- selftest `kbd-layout`

**Note v1:**
- layout attivo globale per la console corrente; il passaggio a stato per-sessione/TTY
  resta un miglioramento futuro, non un blocker della milestone
- la resa UTF-8 del terminale testuale resta legata al path `term80`, che oggi e'
  ancora principalmente byte-oriented

**RT constraint:** la traduzione del tasto resta O(1) con lookup tabellare; il cambio layout
non è RT-safe, ma il path di input ordinario sì.

**Dipende da:** M4-01, M4-02, M4-03, M4-04
**Sblocca:** shell e tool testuali realmente usabili in italiano, login locale, editor TUI,
input coerente anche con tastiere USB future

---

#### M8-08h · i18N / l10n delle Stringhe di Sistema

Gestione internazionale delle stringhe user-visible per shell, utility, server
user-space e configurazione di sistema. Obiettivo minimo iniziale: almeno
**`en_US.UTF-8`** e **`it_IT.UTF-8`**.

- Tutte le stringhe user-facing nuove passano da un layer di cataloghi, non da
  letterali sparsi nel codice
- Base UTF-8 end-to-end: input, output, path, messaggi, prompt e utility
- Variabili locale supportate:
  `LANG`, `LC_ALL`, `LC_MESSAGES`, `LC_TIME`, `LC_NUMERIC`
- Configurazione persistente in `/etc/locale.conf`
- Cataloghi messaggi in `/usr/share/locale/<locale>/LC_MESSAGES/`
- API prevista lato libc/userspace:
  `gettext`-lite o equivalente compatibile con chiavi/catologhi semplici
- Kernel hot path esclusi dalla localizzazione dinamica:
  panic, eccezioni, log hard-RT e path timing-critical restano stringhe fisse
  o message-id stabili per evitare overhead e nondeterminismo
- Localizzazione iniziale richiesta per:
  shell di default, login manager, utility base (`ls`, `cat`, `cp`, `mv`, `rm`,
  messaggi VFS), server di rete principali e pannelli TUI/GUI futuri
- Prevedere anche formattazione locale di data/ora e numeri, senza imporla ai
  task RT o ai log diagnostici low-level

**Scelta progettuale consigliata:** stringhe canonical in inglese come message-id,
traduzioni italiane come primo catalogo completo; l'utente sceglie la lingua via locale.

**Dipende da:** M4-04, M8-08b, M11-01
**Sblocca:** UX multilingua coerente, shell e tool traducibili, setup reale del sistema,
compatibilità con software userspace che si aspetta locale e cataloghi

---

**Struttura file nuovi:**
```
compat/arksh/
    enlilos.c           — platform layer EnlilOS per arksh (contribuito upstream)
    enlilos.h
kernel/syscall.c        — fd object model + pipe()/dup()/dup2()/isatty()
kernel/tty.c            — subset termios canonical/raw (M8-08c)
include/termios.h       — ABI termios minimale condivisa kernel/userspace
user/posix_demo.c       — demo EL0 per pipe/dup/cwd/termios
tools/enlilos-aarch64.cmake  — toolchain file CMake cross-compilation
Makefile                — target 'arksh' aggiunto
```

**Dipende da:** M8-01 (fork), M6-02 (execve), M11-01 (musl libc), M8-03 (signal),
M5-02 (VFS readdir per glob), M4-03 (line discipline per termios)
**Sblocca:** shell interattiva completa con oggetti, pipeline `|>`, syntax highlighting,
history, plugin system — shell di default al boot

---

## MILESTONE 9 — Server Architecture (Hurd-style)

> **Principio:** il kernel EnlilOS non implementa politiche — solo meccanismi.
> Ogni driver di device, filesystem e protocollo di rete è un processo user-space
> che comunica con il kernel e con gli altri server via IPC sincrono (M7-01).
> L'IPC priority donation garantisce che un server che serve un task RT
> esegua temporaneamente alla stessa priorità del chiamante.

---

### ✅ M9-01 · Capability System
**Priorità:** CRITICA — base per tutta la sicurezza dei server

- Ogni risorsa (porta IPC, buffer GPU, file descriptor, mapping MMU) è rappresentata
  da un **capability token**: intero a 64 bit non indovinabile
- Il kernel mantiene una tabella `cap_table[pid][MAX_CAPS]` di capabilities valide
- `cap_send(port, cap)`: trasferisce capability a un altro processo via IPC
- `cap_revoke(cap)`: invalida il token — tutti i detentori perdono accesso
- `cap_derive(cap, rights_mask)`: crea capability figlia con diritti ristretti
- Implementazione: token = `CNTPCT_EL0 ^ (pid << 32) ^ random_salt` al momento della creazione
- Implementato ora: backend kernel statico con `cap_pool[256]`, `cap_table[pid][64]`,
  syscall `cap_alloc/cap_send/cap_revoke/cap_derive/cap_query`, enforcement di owner e rights,
  copia sicura verso user-space per `cap_query()`, token mascherati a 63 bit per ABI EL0 signed-safe,
  demo `/CAPDEMO.ELF` e validazione runtime da `runelf` e `nsh exec`
- Nota RT/onesta: `cap_revoke()` non e' O(1), ma bounded su pool statico
  (`SCHED_MAX_TASKS * MAX_CAPS_PER_TASK`); resta comunque accettabile per il profilo previsto
- Restano per le milestone server successive: uso esteso di `CAP_MMIO`, `CAP_FD`, `CAP_GPU_BUF`
  e trasferimento capability come meccanismo primario tra server user-space
- Selftest automatico `cap-core`: spawna `/CAPDEMO.ELF`, attende zombie, verifica exit code 0

**Numeri syscall (range 60–79):**

| Nr | Nome | RT-safe | Firma |
|----|------|---------|-------|
| 60 | `cap_alloc` | No | `(uint32_t type, uint64_t rights) → cap_t` |
| 61 | `cap_send` | Sì | `(ipc_port_t dst, cap_t c) → int` |
| 62 | `cap_revoke` | Sì — bounded | `(cap_t c) → int` |
| 63 | `cap_derive` | No | `(cap_t src, uint64_t mask) → cap_t` |
| 64 | `cap_query` | Sì — bounded | `(cap_t c, cap_info_t *out) → int` |

---

### ✅ M9-02 · VFS Server in User-Space (bootstrap v1)
**Priorità:** ALTA (migra il VFS kernel di M5-02 fuori dal kernel)

Il VFS kernel di M5-02 è una soluzione di bootstrap: corretto ma monolitico.
M9-02 lo sostituisce con un server user-space ELF separato.

- Server `vfsd` a priorità media: riceve richieste IPC `open/read/write/readdir/stat/close`
- Namespace VFS: mount table gestita dal server, non dal kernel
- Mount dinamico: un processo privilegiato invia `vfs_mount(path, server_port, fstype)` al `vfsd`
- Il kernel fornisce solo l'astrazione IPC: `vfsd` usa `ipc_send/recv` per parlare con `blkd`
- Il kernel smette di contenere codice ext4 o blocco: tutto in user-space

**Passaggio graduale:** M5-02/M5-03 restano attivi finché `vfsd` non è stabile;
poi `vfs_kernel_ops` viene rimosso dal kernel e sostituito con chiamate IPC al server.

**Implementato (v1 bootstrap):**
- server `user-space` reale `/VFSD.ELF`, avviato al boot e bindato alla porta microkernel `vfs`
- protocollo IPC dedicato per `open/read/write/readdir/stat/close`
- syscall minime lato server: `port_lookup`, `ipc_wait`, `ipc_reply`
- bridge kernel-side: i syscall file-oriented dei task EL0 usano `vfsd`, mentre il kernel
  mantiene ancora il backend VFS bootstrap come appoggio interno
- validato a runtime: `nsh` esegue `ls`, `cat /BOOT.TXT`, `cd /data`, `ls`, `cat /data/MREACT.TXT`
  passando attraverso `vfsd`
- selftest automatico `vfsd-core`: verifica porta `vfs` registrata, owner non zombie,
  owner è task user-space, stat di `/VFSD.ELF` passante
- proxy kernel con retry (1s deadline) su EBUSY/EAGAIN per la finestra di avvio del server

**Resta oltre M9-04:**
- rimozione del backend ext4/blocco dal kernel
- mount dinamico reale tra server distinti (`vfsd` ↔ `blkd`) con backend non piu' bootstrap
- cleanup/GC dello stato namespace lato server e policy piu' ricche per i mount

---

### ✅ M9-03 · Block Server in User-Space (`blkd`)
**Priorità:** ALTA (dipende da M9-01)

Migra `drivers/blk.c` (M5-01) fuori dal kernel. Il driver virtio-blk diventa un server:

- `blkd` mappa il MMIO virtio-blk direttamente tramite `mmap(MMIO_PADDR, ...)`
  + capability `CAP_MMIO` che il kernel rilascia solo a processi privilegiati
- Riceve richieste `blk_read(sector, buf, count)` / `blk_write(...)` via IPC
- Risponde con dati + status, trasferendo buffer tramite capability di memoria condivisa
- `blkd` ha priorità bassa: mai invocato direttamente da task hard-RT
- Il task hard-RT lavora su buffer pre-caricati → latenza deterministica

---

### ✅ M9-04 · Namespace & Mount Dinamico
**Priorità:** MEDIA

- syscall `chdir/getcwd`, `mount/umount`, `unshare(CLONE_NEWNS)` e `pivot_root()` proxyate dal kernel verso `vfsd`
- namespace mount per-processo lato server: `vfsd` tiene `cwd` e mount table privata per PID,
  con ereditarieta' su `fork()` tramite `SYS_VFS_BOOT_TASKINFO`
- bind mount `mount(src, dst, "bind", 0)` implementato come alias di namespace verso lo stesso backend
- `pivot_root()` funzionante per il bootstrap: il vecchio root viene ribasato nella nuova vista
  (es. `/mnt/oldroot` -> `/oldroot`) e il task puo' passare da `initrd` a root ext4 privato
- `open/execve/spawn` e il path shadow dei file descriptor usati da `mmap/msync` risolvono i path
  tramite `vfsd`, quindi rispettano la vista privata del processo
- demo `NSDEMO.ELF` e comando boot `nsdemo`
- selftest `vfs-namespace` validato nel run completo `SUMMARY total=25 pass=25 fail=0`

**Note v1 oneste:**
- i backend filesystem reali restano ancora bootstrap kernel-side dietro `SYS_VFS_BOOT_*`
- lo stato client/namespace in `vfsd` non fa ancora garbage collection dei PID terminati
- `pivot_root()` copre il caso di bootstrap e rebase del vecchio root; policy avanzate restano per M11-07

---

## MILESTONE 10 — Network Stack

### ⬜ M10-01 · VirtIO Network Driver
**Priorità:** ALTA

- Probe `virtio-mmio` device ID 1 (virtio-net)
- Negoziazione feature: `VIRTIO_NET_F_MAC`, `VIRTIO_NET_F_STATUS`
- Due vrings: `receiveq` (buffer pre-allocati per RX) + `transmitq` (TX)
- RX IRQ → ring buffer DMA → pacchetto copiato in pool statico → notifica `netd`
- TX: copia frame nel descriptor TX, kick virtio, polling/IRQ per completamento
- API kernel interna: `net_send(buf, len)`, `net_recv(buf, maxlen) → len`
- Server `netd` a priorità media: gestisce il driver e l'IP stack

---

### ⬜ M10-02 · TCP/IP Stack Minimale
**Priorità:** ALTA (dipende da M10-01)

Opzioni:
- **lwIP** (porta): maturo, piccolo, già usato in OS embedded; licenza BSD
- **picotcp**: alternativa più moderna, licenza GPLv2
- **custom minimale**: ARP + IPv4 + UDP + TCP bare, ~3000 righe; massimo controllo WCET

**Decisione consigliata:** porta di **lwIP** in `netd` user-space — evita di reinventare
la gestione dei timer TCP, la ritrasmissione e la gestione delle finestre scorrevoli.

**Componenti:**
- ARP cache (statica, 16 entry)
- IPv4: forward, TTL, checksum HW se supportato da virtio-net
- UDP: send/recv diretto (RT-safe se buffer pre-allocati)
- TCP: slow path (3-way handshake, ritrasmissione, congestion control) — mai da task hard-RT
- ICMP: echo reply (`ping`)

---

### ⬜ M10-03 · BSD Socket API
**Priorità:** ALTA (dipende da M10-02)

**Numeri syscall (range 200–219):**

| Nr | Nome | RT-safe | Note |
|----|------|---------|------|
| 200 | `socket` | No | `AF_INET`/`AF_UNIX`, `SOCK_STREAM`/`SOCK_DGRAM` |
| 201 | `bind` | No | |
| 202 | `listen` | No | |
| 203 | `accept` | No | Bloccante con timeout |
| 204 | `connect` | No | Non-blocking opzionale (`O_NONBLOCK`) |
| 205 | `send` | Sì (UDP pre-alloc) | `flags`: `MSG_DONTWAIT` per path RT |
| 206 | `recv` | Sì (UDP polling) | `MSG_DONTWAIT` ritorna `EAGAIN` se vuoto |
| 207 | `sendto` | Sì | UDP con destinazione esplicita |
| 208 | `recvfrom` | Sì | |
| 209 | `setsockopt` | No | `SO_REUSEADDR`, `SO_RCVTIMEO`, `SO_SNDTIMEO` |
| 210 | `getsockopt` | No | |
| 211 | `shutdown` | Sì | |
| 212 | `getaddrinfo` | No | Implementato interamente in libc; usa `resolv` server |

**AF_UNIX (socket locali):** zero-copy via capability di buffer condiviso — latenza ~2µs
invece dei ~50µs del loopback TCP.

---

## MILESTONE 11 — Porting e Compatibility Layer

### ✅ M11-01 · musl libc
**Priorità:** CRITICA (sblocca ogni programma C esistente)

Porta di **musl libc** come C runtime standard per EnlilOS.

**Stato complessivo attuale:** completata `v1`
- `M11-01a` completata `v1`
- `M11-01b` completata `v1`
- `M11-01c` completata `v1`
- esito runtime attuale dei test collegati: `musl-abi-core`, `tls-tp`, `crt-startup`,
  `musl-hello`, `musl-stdio`, `musl-malloc`, `musl-forkexec`, `musl-pipe` verdi

- `arch/aarch64/`: syscall wrappers AArch64 già presenti in musl, adattati ai numeri EnlilOS
- `sys/`: `open`, `read`, `write`, `close`, `mmap`, `brk`, `exit`, `fork`, `execve`,
  `waitpid`, `getpid`, `kill`, `sigaction`, `socket`, `connect`, ...
- `stdio.h`: `printf`, `fopen`, `fclose` su VFS IPC
- `malloc`: usa `brk()` / `mmap(MAP_ANONYMOUS)` — già implementati
- `pthread.h`: vedi M11-02
- build/runtime `v1`: sysroot bootstrap in-tree (`toolchain/sysroot`), wrapper
  `aarch64-enlilos-musl-*`, `libc.a` statica minimale e smoke test embedded nell'initrd
- porting upstream completo di musl/toolchain esterna: rinviato a un refinement successivo

**Scope reale della v1 (riallineato al GAP):**
- toolchain `aarch64-enlilos-musl-*`
- linking **statico** con `crt1/crti/crtn` + `libc.a`
- startup ABI corretto sopra `argc/argv/envp/auxv`
- runtime C single-thread con `stdio`, `malloc`, `fork/exec/wait`, pipe e termios
- niente `pthread`, `futex`, `dlopen` o linker dinamico user-space in questa milestone

**Prerequisito critico anticipato dal GAP:**
- **TLS base gia' in M11-01**, non in M11-02:
  - supporto `PT_TLS`
  - `TPIDR_EL0` come thread pointer user-space
  - save/restore del TP nei context switch
  - reset/riinizializzazione coerente su `fork()` e `execve()`

**Deliverable attesi:**
- sysroot `musl` per EnlilOS
- wrapper toolchain (`aarch64-enlilos-musl-gcc`, `ar`, `ranlib`)
- 4-6 smoke test musl-linked inseriti in initrd o build separata
- target tipo `make musl-smoke`

**Syscall di compatibilità aggiuntive necessarie:**

| Nr | Nome | Note |
|----|------|-------|
| 39 | `getpid` | Sì — O(1): legge `current->pid` |
| 40 | `getppid` | Sì — O(1) |
| 42 | `gettimeofday` | Alias `clock_gettime(CLOCK_REALTIME)` |
| 43 | `nanosleep` | bounded cooperative sleep con interrupt via signal |
| 44 | `getuid` | Sempre 0 (root) per ora |
| 45 | `getgid` | Sempre 0 |
| 46 | `geteuid` | Sempre 0 |
| 47 | `getegid` | Sempre 0 |

**ABI kernel / libc da aggiungere esplicitamente (emersi dal GAP):**
- `lseek()` — necessaria per `fseek/ftell` e `stdio`
- `readv()` / `writev()` — fortemente raccomandate per ridurre patch invasive su musl
- `fcntl()` minimo: `F_GETFL`, `F_SETFL`, `F_DUPFD`, `F_SETFD`
- `openat()` minimo con `AT_FDCWD`
- `fstatat/newfstatat()` minimo per wrapper libc moderni
- `ioctl()` minimo: default `-ENOTTY`, con crescita futura verso tty/winsize
- `uname()` minimale (`sysname=EnlilOS`, `machine=aarch64`)

**Loader / auxv da completare:**
- verificare e, se necessario, aggiungere `AT_RANDOM`
- valutare `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`

**Sottofasi consigliate per l'implementazione:**

**✅ M11-01a · ABI kernel minima per musl**
- implementata v1 nel kernel con syscall `39–55`
- `getpid/getppid/gettimeofday/nanosleep/getuid/getgid/geteuid/getegid`
- `lseek`, `readv`, `writev`
- `fcntl` v1: `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`, `F_DUPFD`, `F_DUPFD_CLOEXEC`
- `openat` v1 con `AT_FDCWD` e path assoluti
- `fstatat/newfstatat` v1 con `AT_FDCWD` + `AT_EMPTY_PATH`
- `ioctl` v1: `TCGETS`, `TCSETS`, `TIOCGWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`, `FIONBIO`, fallback `-ENOTTY`
- `uname`
- `O_CLOEXEC` / `FD_CLOEXEC` con close-on-exec applicato in `execve()`
- seek coerente anche su fd remoti dietro `vfsd`, tramite estensione IPC `VFSD_REQ_LSEEK`
- demo `/MUSLABI.ELF`, comando boot `muslabi`, selftest `musl-abi-core`

**Limiti v1 dichiarati:**
- `openat()` su dirfd diversi da `AT_FDCWD` resta rinviata
- `fstatat()` non implementa ancora semantica completa `dirfd` relativa oltre `AT_FDCWD`
- `fcntl(F_SETFL)` aggiorna il subset utile al bootstrap libc (`O_NONBLOCK` / `O_APPEND`)

**M11-01b · TLS e startup runtime**
- **Stato attuale:** completata `v1`
- completati `PT_TLS`, `TPIDR_EL0` nel contesto task e ripristino corretto sia al primo ingresso EL0 sia nei resume dopo context switch
- `auxv` estesa con `AT_RANDOM`, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`
- bootstrap C statico completato con `crt1`, `crti`, `crtn`, `environ`, `__enlilos_auxv`, `preinit/init/fini arrays`
- validazione runtime tramite `TLSDEMO.ELF` (`tls-tp`), `MUSLABI.ELF` (`musl-abi-core`) e `CRTDEMO.ELF` (`crt-startup`)

**Note v1:**
- profilo supportato: ELF statici single-thread con TLS locale-exec
- il supporto TLS multi-thread resta nel perimetro di `M11-02`
- layout TLS statico validato col toolchain attuale:
  `[TCB stub 16B][tdata][tbss zeroed/aligned]`, con `TPIDR_EL0` puntato al TCB
- i segmenti `PT_TLS` con `p_memsz == 0` vengono ignorati: alcuni ELF li emettono
  anche senza TLS reale e non devono causare allocazione o fault al bootstrap
- gotcha architetturale chiuso: il restore di `TPIDR_EL0` va fatto dal `current_task`
  reale dopo il ritorno da `sched_context_switch()`, e va impostato esplicitamente
  anche nel primo ingresso EL0 da `sched_task_bootstrap()`

**Deliverable chiusi da considerare done:**
- loader ELF con allocazione/copia TLS statica per `PT_TLS`
- save/restore di `TPIDR_EL0` in scheduler, fork ed exec path
- runtime `crt1/crti/crtn`
- accesso a `environ` e `__enlilos_auxv`
- hook constructor/destructor e `init/fini arrays`
- test end-to-end su file `/data/CRTDEMO.TXT`

**M11-01c · Toolchain e smoke test**
- **Stato attuale:** completata `v1`
- sysroot bootstrap in `toolchain/sysroot/usr/include` + `toolchain/sysroot/usr/lib`
- wrapper `aarch64-enlilos-musl-gcc`, `aarch64-enlilos-musl-ar`, `aarch64-enlilos-musl-ranlib`
- `libc.a` minimale bootstrap costruita da `toolchain/enlilos-musl/src`
- integrazione build con `make musl-sysroot` e `make musl-smoke`
- smoke test statici embedded nell'initrd:
  `MUSLHELLO.ELF`, `MUSLSTDIO.ELF`, `MUSLMALLOC.ELF`, `MUSLFORK.ELF`, `MUSLPIPE.ELF`
- validazione runtime piu' recente nel selftest completo `SUMMARY total=44 pass=44 fail=0`

**Note v1:**
- profilo static-only, single-thread, pensato per bootstrap e smoke test
- il wrapper impone `--sysroot` + include `usr/include` del sysroot EnlilOS
- gli header syscall bootstrap usano un header privato (`enlil_syscalls.h`) per evitare
  collisioni con gli header kernel del repo
- gotcha chiuso nel bring-up: la precedence include per la bootstrap libc deve essere
  `toolchain/enlilos-musl/include` prima di `include/`
- gotcha runtime chiuso dal selftest `musl-pipe`: una pipe deve restituire i byte
  disponibili, non aspettare di riempire tutto il buffer richiesto

---

### 🟡 M11-02 · POSIX Threading (`pthread`)
**Priorità:** ALTA (dipende da M8-01 e M11-01)

Thread implementati sopra `clone()` e `futex`, ma con una correzione
architetturale importante rispetto alla formulazione iniziale del backlog:

- `pthread_create()` non puo' appoggiarsi solo al TCB corrente: serve uno stato
  condiviso di processo (`mm/files/sighand/fs`) per supportare davvero
  `CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD`
- `pthread_join()` non va modellata su `waitpid()`: il path corretto e'
  `CLONE_CHILD_CLEARTID + set_tid_address() + futex(FUTEX_WAIT)`
- `getpid()` deve diventare `tgid`, mentre `gettid()` va aggiunta come syscall dedicata
- `sem_t` POSIX va chiusa sopra `ksem`
- `pthread_mutex_t` / `pthread_cond_t` vanno chiuse sopra `futex`
- TLS multi-thread estende la base gia' introdotta in `M11-01`

**Studio di implementazione dettagliato:** vedi [M11-02.md](M11-02.md)

**Stato reale ad oggi:**
- `M11-02a`, `M11-02b`, `M11-02c`, `M11-02d` e `M11-02e` sono completate `v1`
- baseline kernel gia' presente:
  - `getpid()` = `tgid`, nuova `gettid()`
  - `clone()` subset thread-oriented
  - stato condiviso di processo via `proc_slot` statico (`mm`, fd table, `brk`, namespace/cwd `vfsd`, signal dispositions)
  - selftest `clone-thread` e demo `/CLONEDEMO.ELF`
- lifecycle thread gia' presente:
  - `set_tid_address()`
  - `exit_group()`
  - `tgkill()`
  - `clear_child_tid` con clear affidabile su exit normale e signal-directed terminate
  - processo waitable solo dopo l'uscita dell'ultimo thread
  - selftest `thread-lifecycle` e demo `/THREADLIFE.ELF`
- futex core gia' presente:
  - `FUTEX_WAIT`
  - `FUTEX_WAKE`
  - `FUTEX_REQUEUE`
  - `FUTEX_CMP_REQUEUE`
  - wake su `clear_child_tid`
  - selftest `futex-core` e demo `/FUTEXDEMO.ELF`

**Scope reale consigliato per la `v1`:**
- `pthread_create`
- `pthread_join`
- `pthread_detach`
- `pthread_self`
- `pthread_equal`
- `pthread_kill`
- `pthread_sigmask`
- `pthread_mutex_*` baseline
- `pthread_cond_*` baseline
- `sem_t` POSIX

**Fuori scope `v1`:**
- robust futex list
- `FUTEX_LOCK_PI`
- `pthread_cancel`
- affinity e scheduler attributes completi
- `clone3()`
- process-shared pthread objects

**Prerequisiti tecnici dentro la milestone:**
- gia' chiusi in `M11-02a`:
  - stato condiviso `processo + thread-group`
  - `clone`
  - `gettid`
- ancora aperti:
  - `mprotect` minimale per stack guard page thread

**Proposta numerazione syscall coerente con lo spazio libero attuale:**

| Nr | Syscall | Note |
|----|---------|------|
| 56 | `clone` | subset thread-oriented |
| 57 | `gettid` | TID reale del thread |
| 58 | `set_tid_address` | `clear_child_tid` per join |
| 59 | `exit_group` | distingue process-exit da thread-exit |
| 65 | `futex` | evita collisione con `cap` e `kmon` |
| 66 | `tgkill` | segnale thread-directed |
| 67 | `mprotect` | minimo necessario per guard page |

**Futex (Fast Userspace Mutex):**

| Nr | Nome | RT-safe | Note |
|----|------|---------|------|
| 65 | `futex` | Parziale (`WAKE/REQUEUE`) | `WAIT/WAKE/REQUEUE` base per mutex, condvar e join |

Implementazione kernel:
- Hash table statica di 256 bucket su `(uaddr & 0xFF) * sizeof(futex_bucket_t)`
- Ogni bucket: lista di task bloccati su quella uaddr (senza allocazione)
- `FUTEX_WAIT`: controlla `*uaddr == val`, se uguale blocca su bucket
- `FUTEX_WAKE(n)`: sblocca i primi `n` task nel bucket, ri-schedula
- `FUTEX_REQUEUE` / `FUTEX_CMP_REQUEUE`: supporto consigliato per condvar/broadcast
- `FUTEX_LOCK_PI`: rinviato oltre la `v1`

**Blocchi consigliati:**

**M11-02a · Processo condiviso + `clone()` kernel**
`✅ Completata v1`

- introdotto `tgid`, con `getpid()` riallineato al process id logico
- aggiunta `gettid()` per il TID reale del thread
- introdotto stato condiviso di processo via `proc_slot` statico:
  `mm`, fd table, `brk`, namespace/cwd `vfsd`, signal dispositions
- `clone()` subset thread-oriented implementata per
  `CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD`
- supporto `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`,
  `CLONE_CHILD_CLEARTID` lato memoria utente
- `fork()` e `execve()` rifiutano ancora con `-EBUSY` i processi multi-thread:
  il limite resta intenzionale finche' non esiste un path pthread/futex completo
  e ben validato oltre `M11-02b`
- selftest `clone-thread` verde, demo `/CLONEDEMO.ELF` disponibile dalla boot console

**Limiti dichiarati della v1:**
- limite storico ora chiuso da `M11-02d`: `pthread_join()/detach()` non andavano
  modellate sopra `waitpid()`, ma sopra `clear_child_tid + futex`
- i metadati di processo zombie restano vivi fino al reap per non rompere
  `waitpid()`, `SIGCHLD` e job control

**M11-02b · Lifecycle thread + join groundwork**
- **Stato attuale:** completata `v1`
- `set_tid_address()`
- `exit_group()`
- `tgkill()`
- cleanup differenziato thread/processo
- `clear_child_tid` pulito in modo affidabile su exit normale e terminazione via segnale
- `signal_send_pid()` ormai process-directed per `tgid`; la delivery thread-directed passa da `tgkill()`
- processo marcato waitable solo quando esce l'ultimo thread del thread-group
- demo `/THREADLIFE.ELF`
- selftest `thread-lifecycle`

**Note v1:**
- `pthread_join()` non va ancora costruita sopra `waitpid()`: la base corretta resta
  `CLONE_CHILD_CLEARTID + set_tid_address() + futex(FUTEX_WAIT)`
- `exit_group()` termina il resto del thread-group e fa convergere il codice finale
  del processo sul leader waitable

**M11-02c · Futex core**
- **Stato attuale:** completata `v1`
- `WAIT`
- `WAKE`
- `REQUEUE`
- `CMP_REQUEUE`
- hash table statica per bucket, waiter senza allocazioni dinamiche nel hot path
- key `(proc_slot, uaddr)` coerente col profilo thread shared-mm attuale
- wake su `clear_child_tid` integrato nel path di uscita thread
- demo `/FUTEXDEMO.ELF`
- selftest `futex-core`

**Note v1:**
- `FUTEX_PRIVATE_FLAG` e' accettato come hint ma non cambia la semantica
- timeout `WAIT`, `WAIT_BITSET`, robust list e `FUTEX_LOCK_PI` restano fuori scope
- il supporto cross-process shared futex non e' ancora nel perimetro: la `v1`
  e' keyed per processo (`proc_slot`) e copre bene thread dello stesso address space

**M11-02d · musl `pthread` + `sem_t`**
- **Stato attuale:** completata `v1`
- header bootstrap:
  - `<pthread.h>`
  - `<signal.h>`
  - `<semaphore.h>`
- `pthread_create/join/detach/self/equal/kill/sigmask`
- `pthread_mutex_*` baseline sopra `futex`
- `pthread_cond_*` baseline sopra `futex`
- `sem_t` named/anon sopra `ksem`
- hook bootstrap in `crt1` per inizializzare il runtime thread-aware
- demo `/PTHREADDEMO.ELF`
- demo `/SEMDEMO.ELF`
- selftest `musl-pthread`
- selftest `musl-sem`

**Note v1:**
- `pthread_join()` usa il path corretto `clear_child_tid + futex`, non `waitpid()`
- gli handle `ksem` sono condivisi per `tgid`, cosi' `sem_t` funziona tra thread dello
  stesso processo senza duplicare ref per `tid`
- gotcha chiuso nel bring-up: `sem_timedwait()` vuole un `abstime` valido/non negativo;
  nei selftest iniziali il tempo era vicino a boot `0s`, quindi sottrarre `1s` produceva
  correttamente `EINVAL`, non `ETIMEDOUT`
- il limite storico sul TLS multi-thread e' stato chiuso da `M11-02e`, che alloca un
  blocco TLS completo per ogni thread figlio a partire dal template `PT_TLS`

**M11-02e · Selftest e smoke multi-thread**
- **Stato attuale:** completata `v1`
- chiuso:
  - selftest kernel `clone-thread`
  - selftest kernel `thread-lifecycle`
  - selftest kernel `futex-core`
  - selftest user-space `musl-pthread`
  - selftest user-space `musl-sem`
  - selftest user-space `tls-mt`
  - demo `PTHREADDEMO.ELF`
  - demo `SEMDEMO.ELF`
  - demo `TLSMTDEMO.ELF`
  - coverage esplicita su `__thread` cross-thread e TLS statico completo
  - `errno` thread-local via TLS

**Note v1:**
- i thread figli allocano un blocco TLS completo partendo dal template `PT_TLS`
  descritto in `auxv` (`AT_PHDR/AT_PHNUM`) invece del vecchio stub minimo
- il thread pointer resta compatibile con il layout loader-side:
  `[TCB stub 16B][tdata][tbss]`
- `errno` e' ora `__thread`, quindi la semantica POSIX minima non e' piu' condivisa
  accidentalmente tra i thread dello stesso processo
- restano fuori scope di `M11-02` solo gli aspetti non richiesti dalla `v1`:
  `FUTEX_LOCK_PI`, robust futex list, `pthread_cancel`, affinity e process-shared objects

---

### ✅ M11-03 · Dynamic Linker / `libdl` bootstrap
**Priorità:** completata `v1`

**Deliverable chiusi nella v1:**
- syscall kernel-side `dlopen/dlsym/dlclose/dlerror` (`67–70`)
- registry per-process dei moduli runtime in `elf_loader`
- load runtime di oggetti `ET_DYN` nel processo corrente
- risoluzione `DT_NEEDED` e relocation bootstrap per il profilo gia' supportato dal loader
- `libdl.a` bootstrap nel sysroot musl con `<dlfcn.h>`
- smoke `/MUSLDL.ELF` e selftest `musl-dlfcn`

**Stato pratico raggiunto:**
- `dlopen("/libdyn.so", RTLD_NOW)` e `dlsym()` funzionano in EL0
- il dynamic linker `v1` sblocca davvero il port hosted di `arksh`, che ora supera il
  vecchio blocco su `<dlfcn.h>` e linka il binario reale statico host-side

**Limiti onesti della v1:**
- `dlclose()` rilascia il handle logico ma non smappa ancora le pagine runtime
- niente lazy binding/PLT avanzato
- niente ASLR serio per le shared library
- risoluzione simboli intenzionalmente minima: profilo bootstrap sufficiente a smoke e port shell

---

### ⬜ M11-04 · Mach-O Compatibility Layer
**Priorità:** MEDIA (dipende da M11-01 e M11-03)

Supporto per eseguire binari Mach-O AArch64 (prodotti da Xcode / macOS toolchain)
su EnlilOS senza ricompilazione. Il layer traduce le differenze di ABI, formato
binario e syscall tra XNU e EnlilOS.

**Contesto:** su AArch64 macOS i binari usano:
- Formato **Mach-O 64-bit** (`magic = 0xFEEDFACF`) o **fat binary** (`0xCAFEBABE`)
- `svc #0x80` per syscall BSD Unix (stile XNU), con numero syscall in `x16`
- Mach traps via `svc #0` con numero trap negativo in `x16` (es. `mach_msg` = -31)
- `libSystem.dylib` come runtime C (wrappa tutti i syscall XNU)
- `dyld` come dynamic linker; entrypoint da `LC_MAIN` (non `e_entry` ELF)

**Obiettivo minimo:** eseguire binari C/C++ statici o con sole dipendenze da
`libSystem.dylib` senza richiedere un porting. Objective-C runtime e framework
Apple (`Foundation`, `UIKit`, ecc.) sono fuori scope per questa milestone.

---

#### M11-04a · Mach-O Loader

- Rilevamento magic al `execve()`: se `*(uint32_t*)buf == 0xFEEDFACF` → loader Mach-O
- **Fat binary** (`0xCAFEBABE`): scansione degli arch slice, selezione `CPU_TYPE_ARM64`
- Parsing load commands:
  - `LC_SEGMENT_64`: mappa ogni segmento con permessi R/W/X corretti (come `PT_LOAD` ELF)
  - `LC_MAIN`: legge `entryoff` → entrypoint = `__TEXT` base + `entryoff`
  - `LC_LOAD_DYLIB`: colleziona lista `.dylib` richieste → passata al linker Mach-O (M11-04c)
  - `LC_DYLD_INFO_ONLY` / `LC_DYLD_EXPORTS_TRIE`: tabelle rebase/bind per ASLR
  - `LC_UUID`: ignorato
  - `LC_CODE_SIGNATURE`: ignorato (no codesign enforcement)
- **ASLR Mach-O**: `__TEXT` caricato a indirizzo random nella regione `MACHO_LOAD_BASE`
  (distinto dall'ELF range per evitare collisioni)
- **Stack setup XNU**: ABI stack macOS AArch64 = ABI Linux AArch64 → riuso di `elf_setup_stack()`
  con piccoli aggiustamenti (`_mh_execute_header` pointer nell'ambiente)

```c
/* Struttura interna del loader */
typedef struct {
    uintptr_t   text_base;      /* indirizzo caricamento __TEXT */
    uintptr_t   entry;          /* entrypoint dopo rebase */
    uintptr_t   dyld_base;      /* base dyld shim caricato */
    uint32_t    ncmds;
    uint32_t    flags;          /* MH_PIE, MH_DYLDLINK, ... */
} macho_load_info_t;
```

---

#### M11-04b · XNU Syscall Translation

Il meccanismo chiave: quando un binario Mach-O esegue `svc #0x80`, il vettore
di eccezione SVC di EnlilOS controlla il **tipo di syscall**:

- `svc #0x80` con `x16 > 0` → BSD syscall XNU → traduzione via `xnu_bsd_table[]`
- `svc #0x80` con `x16 < 0` → Mach trap → gestione separata (M11-04d)
- `svc #0` → syscall EnlilOS nativa (invariato)

**Tabella di traduzione `xnu_bsd_table[512]`:**

| XNU nr | Nome XNU | EnlilOS nr | Note |
|--------|----------|------------|------|
| 1 | `exit` | 3 | |
| 3 | `read` | 2 | |
| 4 | `write` | 1 | |
| 5 | `open` | 4 | |
| 6 | `close` | 5 | |
| 20 | `getpid` | 39 | |
| 48 | `getpid` (alias) | 39 | |
| 54 | `ioctl` | stub ENOTTY | |
| 97 | `socket` | 200 | |
| 98 | `connect` | 204 | |
| 116 | `gettimeofday` | 96 | |
| 194 | `mmap` | 7 | adattamento flags |
| 197 | `munmap` | 8 | |
| 198 | `mprotect` | stub OK | |
| 202 | `sysctl` | stub parziale | |
| 266 | `shm_open` | stub ENOSYS | |
| 286 | `pthread_sigmask` | → blocksig | |
| 329 | `psynch_mutexwait` | → futex | mapping futex (M11-02) |
| 330 | `psynch_mutexdrop` | → futex | |
| 340 | `psynch_cvwait` | → futex | |
| 341 | `psynch_cvsignal` | → futex | |
| 344 | `psynch_cvbroad` | → futex | |

Implementazione:
- `xnu_bsd_table[]`: array sparso di 512 entry, init al boot, O(1) lookup
- Entry vuota → `ENOSYS` con log una tantum (evita spam per app che probano syscall)
- Argomenti: layout registri XNU (`x0–x7`) = layout EnlilOS → nessuna conversione
- Differenze di flags `open(O_*)`: `O_CREAT=0x200` su XNU vs `0x40` su Linux → maschera di conversione

---

#### M11-04c · dyld Shim (`libdyld-enlil.dylib`)

Invece di portare il `dyld` di Apple (chiuso + complesso), si fornisce uno shim minimo
che soddisfa il contratto atteso dai binari Mach-O:

- `dyld_stub_binder`: risolve i simboli PLT al primo accesso (lazy binding)
- Tabella dei simboli esportati: cerca nell'ordine: `libSystem-enlil.dylib` → altre `.dylib` mappate
- `_dyld_get_image_vmaddr_slide()`: ritorna l'ASLR slide del binario principale
- `_dyld_register_func_for_add_image()` / `_remove_image()`: callback hooks per C++ runtime
- Supporto `@rpath`, `@executable_path`, `@loader_path` per risoluzione path `.dylib`
- File: `compat/macho/dyld_shim.c` + `compat/macho/dyld_shim.h`

---

#### M11-04d · Mach Traps Minimali

I Mach trap sono la IPC nativa di XNU. La maggior parte dei binari C puri li usano poco,
ma il C++ runtime e `libSystem` li usano per inizializzazione e threading.

Trap essenziali (via `svc #0` con `x16 < 0`):

| Trap nr | Nome | Comportamento EnlilOS |
|---------|------|-----------------------|
| -26 | `mach_reply_port` | Ritorna porta IPC fittizia (handle) |
| -27 | `thread_self_trap` | Ritorna TID corrente |
| -28 | `task_self_trap` | Ritorna PID corrente |
| -29 | `host_self_trap` | Ritorna 1 (host port stub) |
| -31 | `mach_msg_trap` | Implementazione parziale (vedi sotto) |
| -36 | `semaphore_signal_trap` | → `futex(FUTEX_WAKE, 1)` |
| -37 | `semaphore_wait_trap` | → `futex(FUTEX_WAIT)` con timeout |
| -89 | `mach_timebase_info` | Popola `numer=1, denom=1` (ns diretti) |
| -90 | `mach_wait_until` | Busy-wait via `CNTPCT_EL0` come `timer_delay_us()` |

`mach_msg_trap` (trap -31) è il messaggio Mach: implementato come stub che gestisce
solo i pattern di startup comuni (`MACH_SEND_MSG + MACH_RCV_MSG` verso task/host self).
Le chiamate non gestite ritornano `MACH_SEND_INVALID_DEST` (errore non fatale).

---

#### M11-04e · libSystem Shim (`libSystem-enlil.dylib`)

`libSystem.dylib` su macOS è la libreria C + runtime thread + Mach stubs. Lo shim
reindirizza le sue funzioni sulle implementazioni musl/EnlilOS:

| Simbolo `libSystem` | Implementazione EnlilOS |
|---------------------|------------------------|
| `malloc` / `free` | musl malloc |
| `pthread_create` | M11-02 pthread |
| `pthread_mutex_*` | futex (M11-02) |
| `dispatch_*` (GCD) | stub minimo (serializza su thread pool) |
| `CFRunLoop*` | stub: esegue una iterazione sincrona |
| `os_log_*` | redirige su `write(2, ...)` UART |
| `arc4random` | legge `CNTPCT_EL0` XOR con salt boot |

**Grand Central Dispatch (GCD) stub:** `dispatch_async` invia un job a un thread pool
statico di 4 worker; `dispatch_sync` esegue inline. Non è performante ma è sufficiente
per far partire binari che usano GCD per inizializzazione.

---

**Struttura file:**

```
compat/macho/
    macho_loader.c      — parsing + mapping segmenti Mach-O
    macho_fat.c         — fat binary slice selection
    xnu_syscall.c       — tabella traduzione BSD syscall XNU→EnlilOS
    xnu_mach_trap.c     — implementazione Mach trap minimali
    dyld_shim.c         — dyld stub binder + immagine lookup
    libsystem_shim.c    — shim libSystem (malloc, pthread, GCD, os_log)
include/macho.h         — strutture pubbliche + magic defines
```

**Limitazioni note:**
- Objective-C runtime (`libobjc.dylib`): non supportato in questa milestone
- Swift runtime: non supportato
- Framework Apple (`Foundation`, `AppKit`, ecc.): fuori scope
- Entitlements e code signing: ignorati
- `kevent` / `kqueue`: stub `ENOSYS` (richiede una milestone dedicata opzionale)
- Fat binary con slice `arm64e` (pointer auth): lo slice `arm64` viene preferito;
  se assente → errore `EBADARCH`

**Dipende da:** M6-01 (page table setup), M11-01 (musl libc), M11-02 (futex + pthread),
M11-03 (dynamic linker ELF come riferimento architetturale)
**Sblocca:** esecuzione di CLI tools macOS AArch64, porting di app C/C++ senza recompila

---

### ⬜ M11-05 · Linux AArch64 Compatibility Layer
**Priorità:** ALTA

> **Vantaggio strutturale unico:** EnlilOS usa già l'ABI syscall Linux AArch64 —
> stessi numeri, stessa convenzione (`svc #0`, nr in `x8`, args in `x0–x5`, ret in `x0`).
> Non serve una tabella di traduzione come per Mach-O (M11-04).
> Il layer di compatibilità Linux è principalmente:
> 1. Completare le syscall Linux non ancora implementate
> 2. Fornire `ld-linux-aarch64.so.1` per i binari dinamici
> 3. Simulare il filesystem `/proc`, `/sys`, `/dev` che Linux glibc si aspetta
> 4. Gestire le differenze semantiche minori (clone flags, ioctl, ecc.)

**Target:** binari ELF AArch64 compilati per Linux con glibc o musl.
Casi d'uso tipici: `bash`, `python3`, `gcc`, `git`, `curl`, strumenti GNU coreutils.

---

#### M11-05a · Syscall Linux Mancanti

EnlilOS ha già i numeri Linux — mancano solo le implementazioni delle syscall
non ancora pianificate. Lista completa di quelle richieste dai binari Linux comuni:

**Nota di riallineamento dal GAP M11-01:**
- alcune primitive che in origine vivevano solo in questa sezione vengono anticipate
  in `M11-01` perché servono già al bootstrap musl: `ioctl` minimo, `uname`,
  `fcntl` minimo, `openat` minimo
- il GAP ha anche reso espliciti prerequisiti prima non visibili nel backlog:
  `lseek`, `readv/writev`, `newfstatat/fstatat` e verifica `auxv` (`AT_RANDOM`)
- in questa milestone resta il completamento ABI Linux più ampio, non il primo supporto libc

| Nr Linux | Nome | Stato EnlilOS | Note implementazione |
|---|---|---|---|
| 17 | `getcwd` | ✅ M8-08b v1 | syscall EnlilOS disponibile come `SYS_GETCWD` |
| 22 | `pipe` | ✅ M8-08a v1 | syscall EnlilOS disponibile come `SYS_PIPE` |
| 23 | `select` | ⬜ questa milestone | fd multipli con timeout |
| 24 | `sched_yield` | ⬜ | chiama `schedule()` — triviale |
| 29 | `shmget` | ⬜ | shared memory System V |
| 30 | `shmat` | ⬜ | attach shared memory |
| 31 | `shmctl` | ⬜ | controllo + IPC_RMID |
| 32 | `dup` | ✅ M8-08a v1 | syscall EnlilOS disponibile come `SYS_DUP` |
| 33 | `dup2` | ✅ M8-08a v1 | syscall EnlilOS disponibile come `SYS_DUP2` |
| 35 | `nanosleep` | ⬜ M11-01 | pianificata |
| 41 | `socket` | ⬜ M10-03 | pianificata |
| 42 | `connect` | ⬜ M10-03 | pianificata |
| 49 | `bind` | ⬜ M10-03 | pianificata |
| 50 | `listen` | ⬜ M10-03 | pianificata |
| 51 | `accept` | ⬜ M10-03 | pianificata |
| 54 | `setsockopt` | ⬜ M10-03 | pianificata |
| 55 | `getsockopt` | ⬜ M10-03 | pianificata |
| 61 | `wait4` | ⬜ | `waitpid` + `rusage` stub |
| 63 | `uname` | ⬜ M11-01 | minimale per bootstrap libc; ABI Linux completa qui |
| 64 | `semget` | ⬜ | System V sem — wrappa ksem (M8-06) |
| 65 | `semop` | ⬜ | System V semop — wrappa ksem |
| 66 | `semctl` | ⬜ | controllo semaforo System V |
| 72 | `fcntl` | ⬜ M11-01 | subset libc in M11-01, estensioni/compat Linux qui |
| 73 | `flock` | ⬜ | advisory lock su fd — stub OK |
| 74 | `fsync` | ⬜ | flush dirty pages al VFS server |
| 76 | `truncate` | ⬜ | tronca file a N byte |
| 77 | `ftruncate` | ⬜ | come truncate ma su fd |
| 78 | `getdents64` | ⬜ | readdir POSIX → formato Linux dirent64 |
| 80 | `chdir` | ✅ M8-08b v1 | syscall EnlilOS disponibile come `SYS_CHDIR` |
| 82 | `rename` | ⬜ M5-04 | già pianificata |
| 83 | `mkdir` | ⬜ M5-04 | già pianificata |
| 84 | `rmdir` | ⬜ M5-04 | già pianificata |
| 87 | `unlink` | ⬜ M5-04 | già pianificata |
| 88 | `symlink` | ⬜ | crea symlink nel VFS |
| 89 | `readlink` | ⬜ | legge target del symlink |
| 92 | `chown` | ⬜ | stub OK (EnlilOS single-user) |
| 93 | `fchown` | ⬜ | stub OK |
| 94 | `lchown` | ⬜ | stub OK |
| 101 | `ptrace` | ⬜ | stub EPERM — sicurezza |
| 107 | `gettimeofday` | ⬜ M11-01 | pianificata |
| 108 | `getrusage` | ⬜ | ritorna runtime_ns dal TCB |
| 110 | `getppid` | ⬜ M11-01 | pianificata |
| 113 | `setuid` | ⬜ | stub OK (single-user) |
| 114 | `setgid` | ⬜ | stub OK |
| 131 | `tgkill` | ✅ M11-02b | invia segnale a thread specifico |
| 160 | `settimeofday` | ⬜ | privilegiato — solo root |
| 203 | `sched_setaffinity` | ⬜ | dopo M13-02 SMP; stub OK prima |
| 204 | `sched_getaffinity` | ⬜ | ritorna maschera CPU disponibili |
| 218 | `set_tid_address` | ✅ M11-02b | clear-child-tid per thread join |
| 220 | `clone3` | ⬜ | estensione di clone() per thread (M11-02) |
| 222 | `mmap` | ✅ M3-02 | già implementata |
| 226 | `mprotect` | ⬜ | cambia permessi MMU su range |
| 233 | `epoll_create1` | ⬜ | event polling scalabile (vedi sotto) |
| 235 | `epoll_ctl` | ⬜ | registra fd in epoll |
| 236 | `epoll_wait` | ⬜ | attende eventi su fd multipli |
| 260 | `wait4` | ⬜ | alias |
| 261 | `faccessat` | ⬜ | access con dirfd |
| 263 | `unlinkat` | ⬜ | unlink con dirfd |
| 267 | `readlinkat` | ⬜ | readlink con dirfd |
| 277 | `openat` | ⬜ M11-01 | minimo libc in M11-01, dirfd/compat avanzata qui |
| 280 | `utimensat` | ⬜ | aggiorna timestamp file |
| 291 | `epoll_create` | ⬜ | alias epoll_create1 |

**Priorità implementazione:** le syscall con `⬜ questa milestone` sono quelle che
bloccano glibc e i binari comuni ma non sono ancora in nessuna altra milestone.

---

#### M11-05b · epoll — Event Polling Scalabile

`epoll` è richiesto da ogni server Linux moderno (`nginx`, `node`, `python asyncio`).
Rimpiazza `select`/`poll` con O(1) per evento invece di O(N) su tutti i fd.

```c
/* Kernel: set epoll statico per processo */
typedef struct {
    int          fd;
    uint32_t     events;     /* EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET */
    uint64_t     data;       /* user data opaco */
} epoll_event_t;

typedef struct {
    epoll_event_t  interests[MAX_EPOLL_FDS];  /* fd registrati */
    uint32_t       n;
    sched_tcb_t   *waiter;
} epoll_set_t;

#define MAX_EPOLL_SETS  16
#define MAX_EPOLL_FDS   256
static epoll_set_t epoll_pool[MAX_EPOLL_SETS];
```

- `epoll_create1(flags)` — alloca un `epoll_set_t` dal pool; ritorna fd "epoll"
- `epoll_ctl(epfd, EPOLL_CTL_ADD/MOD/DEL, fd, event)` — modifica il set
- `epoll_wait(epfd, events, maxevents, timeout_ms)`:
  - Itera sugli fd registrati, controlla disponibilità (read/write ready)
  - Se nessuno pronto + timeout > 0: blocca con `sched_block(timeout_ms * 1_000_000)`
  - Il VFS server notifica l'epoll set quando un fd diventa readable/writable via callback
  - Edge-triggered (`EPOLLET`): notifica solo alla transizione not-ready → ready

---

#### M11-05c · System V IPC (shmem, semafori)

I binari Linux spesso usano System V IPC per shared memory inter-processo.
EnlilOS lo implementa come thin wrapper sopra le primitive native:

**Shared Memory System V (`shmget`/`shmat`/`shmctl`):**
- `shmget(key, size, flags)` → alloca `phys_alloc_pages(order)` + registra in `shm_table[key]`
- `shmat(shmid, addr, flags)` → mappa le pagine nell'address space del chiamante via MMU
- `shmctl(shmid, IPC_RMID, ...)` → decrementa refcount; libera quando zero
- Pool statico: `shm_table[64]` — max 64 segmenti shared memory simultanei
- Implementazione naturale sopra l'MMU identity-map già presente (M1-02)

**Semafori System V (`semget`/`semop`/`semctl`):**
- `semget(key, nsems, flags)` → crea array di `nsems` ksem (M8-06) con chiave IPC
- `semop(semid, sops, nsops)` → esegue array di P/V operazioni atomicamente
- `semctl(semid, semnum, IPC_RMID)` → distrugge il set

---

#### M11-05d · ld-linux-aarch64.so.1 Shim

I binari Linux dinamici cercano `/lib/ld-linux-aarch64.so.1` come dynamic linker.
Due approcci:

**Approccio A (consigliato): symlink verso il linker EnlilOS nativo**
```
/lib/ld-linux-aarch64.so.1 → /lib/ld-enlilos.so  (M11-03)
```
Funziona se il linker EnlilOS (M11-03) rispetta l'interfaccia `PT_INTERP` standard,
che è identica tra Linux e EnlilOS (stesso ELF ABI AArch64).

**Approccio B: ld-linux-musl passthrough**
`musl-libc` fornisce `ld-musl-aarch64.so.1` che può essere rinominato/symlinkato
come `ld-linux-aarch64.so.1`. I binari compilati con musl su Linux girano direttamente.
Per binari glibc: musl implementa una compatibilità sufficiente per la maggior parte dei casi.

**Procedura al boot:**
```
/lib/
    ld-linux-aarch64.so.1  → ld-musl-aarch64.so.1  (symlink)
    ld-musl-aarch64.so.1   (musl dynamic linker da M11-03)
    libc.so.6              → libmusl.so  (symlink — glibc name compat)
    libm.so.6              → libmusl.so  (math è parte di musl)
    libpthread.so.0        → libmusl.so  (pthread è parte di musl)
    libdl.so.2             → libmusl.so  (dlopen è parte di musl)
```

---

#### M11-05e · Linux Filesystem Environment

I binari Linux si aspettano certi path e file nel filesystem. Il layer `procfs/sysfs`
di M14-01 viene esteso per fornirli:

**`/proc` — richiesto da glibc e molti tool:**
```
/proc/self/exe          → symlink all'ELF del processo corrente
/proc/self/maps         → mappa di memoria (estensione M14-01)
/proc/self/fd/          → file descriptor aperti (estensione M14-01)
/proc/self/status       → già nel procfs core M14-01
/proc/self/cmdline      → argv[0] + '\0' + argv[1] + '\0' + ...
/proc/self/environ      → variabili d'ambiente concatenate
/proc/cpuinfo           → "processor : 0\nBogoMIPS : 62.50\nFeatures : fp asimd..."
/proc/meminfo           → "MemTotal: 524288 kB\nMemFree: ..."
/proc/version           → "EnlilOS 1.0.0 (gcc) #1 SMP"
/proc/sys/kernel/pid_max → 32768
/proc/sys/vm/overcommit_memory → 0
```

**`/etc` — richiesto da glibc resolver, NSS, locale:**
```
/etc/hostname           → "enlilos"
/etc/hosts              → "127.0.0.1 localhost\n::1 localhost"
/etc/resolv.conf        → "nameserver 1.1.1.1"  (dopo M10-02)
/etc/passwd             → "root:x:0:0:root:/root:/bin/arksh"
/etc/group              → "root:x:0:"
/etc/os-release         → "NAME=EnlilOS\nID=enlilos\nVERSION_ID=1.0"
/etc/nsswitch.conf      → "passwd: files\nhosts: files dns"
/etc/locale.conf        → "LANG=en_US.UTF-8" oppure "LANG=it_IT.UTF-8"
/etc/localtime          → symlink → /usr/share/zoneinfo/UTC
/etc/ld.so.cache        → cache percorsi .so (generata da ldconfig stub)
/etc/ld.so.conf         → "/lib\n/usr/lib\n"
```

**`/dev` — device nodes:**
```
/dev/null               → già M3-02 (fd speciale — estendere come device node)
/dev/zero               → read → zeri infiniti, write → discard
/dev/urandom            → read → pseudo-random da CNTPCT_EL0 XOR state
/dev/random             → alias /dev/urandom (EnlilOS non blocca su entropia)
/dev/tty                → fd della console corrente
/dev/stdin              → symlink /proc/self/fd/0
/dev/stdout             → symlink /proc/self/fd/1
/dev/stderr             → symlink /proc/self/fd/2
/dev/pts/0              → pseudo-terminal (dopo M11-05f)
```

---

#### M11-05f · Pseudo-Terminal (pty)

Necessario per `ssh`, terminali grafici, `tmux`, `screen`.

- `posix_openpt()` / `grantpt()` / `unlockpt()` / `ptsname()` — API POSIX
- Syscall `openat("/dev/ptmx")` → crea coppia master/slave pty
- Master (`/dev/ptm`) + slave (`/dev/pts/N`): ogni write master → read slave e vice versa
- Slave si comporta come UART con termios completo (M8-08c)
- `ioctl(fd, TIOCGWINSZ/TIOCSWINSZ)` — dimensioni finestra terminale
- Integrazione con arksh (M8-08): arksh usa pty come terminale virtuale per i sotto-processi

---

#### M11-05g · glibc Compatibility Shims

Alcuni simboli glibc non sono in musl o hanno semantica leggermente diversa.
Libreria shim `libglibc-compat.so` (piccola, ~200 righe):

```c
/* Simboli glibc che musl non esporta o esporta diversamente */

/* __libc_start_main — entry point glibc, diverso da musl */
int __libc_start_main(int (*main)(int,char**,char**),
                      int argc, char **argv, ...) {
    /* chiama musl __libc_start_main con la stessa firma */
}

/* gnu_get_libc_version — ritorna versione glibc fittizia */
const char *gnu_get_libc_version(void) { return "2.38"; }

/* __cxa_thread_atexit_impl — C++ thread-local destructors */
/* → rimappato su musl __cxa_thread_atexit */

/* pthread_atfork — glibc lo esporta, musl lo nasconde */
int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void));

/* __stack_chk_fail, __stack_chk_guard — stack protector */
/* → already in musl, ma alcuni binari li linkano esplicitamente */

/* versioned symbols: read@@GLIBC_2.17 → read */
/* Il linker risolve tramite .gnu.version_d section */
```

**Symbol versioning:**
glibc usa versioned symbols (`read@@GLIBC_2.17`). Il dynamic linker (M11-03) esteso
per ignorare la versione e risolvere solo per nome — sufficiente per la maggior parte
dei binari.

---

**Limitazioni note:**
- Binari che usano `clone()` con flags avanzati (`CLONE_NEWNS`, `CLONE_NEWPID`) → stub EPERM (namespace non supportati)
- `io_uring` — non implementato; stub ENOSYS
- `seccomp` — stub ENOSYS (EnlilOS usa capability, non seccomp)
- `perf_event_open` — stub ENOSYS
- `bpf` — stub ENOSYS
- Binari x86-64 compilati per Linux — non girano (richiedono emulazione x86, fuori scope)
- Binari arm64e (pointer auth) — non supportati (come M11-04)

**Struttura file:**
```
compat/linux/
    linux_fs.c          — /proc, /etc, /dev entries (serve estendere M14-01)
    linux_sysv_ipc.c    — shmget/shmat/shmctl + semget/semop/semctl
    linux_epoll.c       — epoll_create/ctl/wait
    linux_pty.c         — pseudo-terminal
    linux_syscall_ext.c — syscall mancanti (select, fcntl, flock, getdents64, ...)
    glibc_compat.c      — shim simboli glibc
include/linux_compat.h  — flag, strutture (epoll_event, dirent64, ipc_perm, ...)
```

**Dipende da:** M6-01 (ELF loader), M11-01 (musl libc), M11-03 (dynamic linker),
M8-08a (pipe/dup), M8-08b (getcwd/chdir), M8-06 (ksem per System V sem),
M14-01 (procfs/sysfs per `/proc` entries)
**Sblocca:** esecuzione di binari Linux AArch64 glibc e musl senza ricompilazione —
`bash`, `python3`, `git`, `curl`, `gcc`, GNU coreutils

---

### ⬜ M11-06 · io_uring
**Priorità:** BASSA
**Dipende da:** M11-05 (Linux compat), M10-03 (socket API), M5-03 (ext4 read path)

I/O asincrono ad alto throughput tramite ring buffer condiviso kernel/user-space.
Elimina il costo delle syscall per I/O intenso: l'app deposita richieste nel ring
senza entrare nel kernel; il kernel le drena in background.

**Caso d'uso concreto su EnlilOS:** `postgres`, `nginx`, `io_uring`-based servers
che su Linux ottengono 2-4× il throughput di `epoll` su I/O intenso grazie a
zero-syscall per richiesta.

**Perché bassa priorità:** `epoll` (M11-05b) + `mreact` (M8-05) coprono il 90%
dei workload. `io_uring` è un'ottimizzazione di performance, non di funzionalità.

---

#### Architettura

Due ring buffer condivisi tra kernel e user-space (mappati via `mmap`):

```c
/* Submission Queue (SQ): l'app scrive qui le richieste */
typedef struct {
    uint8_t   opcode;        /* IORING_OP_READ, WRITE, ACCEPT, SEND, RECV, ... */
    uint8_t   flags;
    uint16_t  ioprio;
    int32_t   fd;
    uint64_t  off;           /* offset file (per read/write) */
    uint64_t  addr;          /* buffer user-space */
    uint32_t  len;
    uint32_t  op_flags;      /* O_NONBLOCK, SPLICE_F_FD_IN_FIXED, ... */
    uint64_t  user_data;     /* opaco — ritornato nella CQ entry */
} io_uring_sqe_t;            /* Submission Queue Entry — 64 byte */

/* Completion Queue (CQ): il kernel scrive qui i risultati */
typedef struct {
    uint64_t  user_data;     /* copiato dalla SQE */
    int32_t   res;           /* valore di ritorno (byte letti/scritti o -errno) */
    uint32_t  flags;
} io_uring_cqe_t;            /* Completion Queue Entry — 16 byte */

/* Struttura condivisa (mappata in user-space) */
typedef struct {
    uint32_t  sq_head;       /* consumato dal kernel */
    uint32_t  sq_tail;       /* prodotto dall'app */
    uint32_t  sq_mask;       /* size-1 per wrap-around */
    uint32_t  cq_head;       /* consumato dall'app */
    uint32_t  cq_tail;       /* prodotto dal kernel */
    uint32_t  cq_mask;
    uint32_t  flags;         /* IORING_SQ_NEED_WAKEUP, IORING_CQ_OVERFLOW */
} io_uring_sq_ring_t;
```

---

#### Syscall (range 425–427 — numeri Linux reali)

| Nr | Nome | Firma C |
|----|------|---------|
| 425 | `io_uring_setup` | `(uint32_t entries, io_uring_params_t *params) → int` — ritorna fd |
| 426 | `io_uring_enter` | `(int fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags) → int` |
| 427 | `io_uring_register` | `(int fd, uint32_t opcode, void *arg, uint32_t nr_args) → int` |

**`io_uring_setup`:**
- Alloca due ring in memoria kernel-condivisa (una pagina ciascuno)
- `mmap` la regione in user-space con permessi R/W (SQ) e R/O (CQ per l'app)
- Ritorna un fd che rappresenta l'istanza io_uring
- Pool statico: `MAX_URING_INSTANCES = 8` per processo

**`io_uring_enter`:**
- `to_submit > 0`: kernel consuma le SQE pendenti, le esegue (o le mette in flight)
- `min_complete > 0`: blocca il task finché `min_complete` CQE non sono disponibili
- Flag `IORING_ENTER_SQ_WAKEUP`: sveglia il kernel worker thread (modalità polling)

---

#### Opcode supportati (subset iniziale)

| Opcode | Nome | Comportamento |
|--------|------|---------------|
| 0 | `IORING_OP_NOP` | no-op, utile per test |
| 1 | `IORING_OP_READV` | readv su fd (file o socket) |
| 2 | `IORING_OP_WRITEV` | writev su fd |
| 3 | `IORING_OP_FSYNC` | fsync su fd |
| 6 | `IORING_OP_READ_FIXED` | read con buffer pre-registrato (zero-copy) |
| 7 | `IORING_OP_WRITE_FIXED` | write con buffer pre-registrato |
| 13 | `IORING_OP_ACCEPT` | accept su socket non-blocking |
| 14 | `IORING_OP_ASYNC_CANCEL` | cancella SQE in volo |
| 16 | `IORING_OP_SEND` | send su socket |
| 17 | `IORING_OP_RECV` | recv su socket |
| 18 | `IORING_OP_OPENAT` | open asincrono |
| 19 | `IORING_OP_CLOSE` | close asincrono |
| 22 | `IORING_OP_STATX` | stat asincrono |

**Buffer pre-registrati (`io_uring_register` + `IORING_REGISTER_BUFFERS`):**
- L'app registra N buffer una volta; il kernel li mappa e blocca in memoria
- Le SQE usano indice invece di puntatore → zero-copy garantito

---

#### Implementazione kernel

- **Worker thread kernel** (`io_uring_worker`): task kernel a priorità `SCHED_OTHER`
  che drena la SQ, esegue le operazioni I/O e scrive le CQE
- **Polling mode** (`IORING_SETUP_SQPOLL`): worker in busy-wait sulla SQ per ~1ms,
  poi dorme — latenza quasi zero senza syscall
- **Link chain** (`IOSQE_IO_LINK`): SQE eseguite in sequenza — la seconda parte solo
  se la prima ha successo; utile per read-then-process pipeline
- **Timeout** (`IORING_OP_LINK_TIMEOUT`): cancella la SQE precedente se scade
- WCET: non garantito (I/O è inherentemente non-deterministico) — `io_uring` non
  è mai usato da task hard-RT; usato solo da server a priorità bassa

**Struttura file:**
```
kernel/io_uring.c       — setup, enter, register, worker thread
include/io_uring.h      — strutture pubbliche SQE/CQE/params
```

---

### ⬜ M11-07 · Namespace e Container Support
**Priorità:** BASSA
**Dipende da:** M11-05 (Linux compat), M9-04 (mount dinamico), M10-01 (virtio-net)

Isolamento delle risorse per processo tramite namespace Linux — base di Docker,
Podman, LXC e qualsiasi container runtime.

**Caso d'uso concreto su EnlilOS:** far girare container Docker/OCI AArch64 nativamente,
isolare processi in sandbox sicure senza VM.

---

#### Namespace supportati

| Namespace | Flag `clone()` | Cosa isola | Complessità |
|---|---|---|---|
| **Mount** | `CLONE_NEWNS` | Vista del filesystem VFS | Media |
| **PID** | `CLONE_NEWPID` | Spazio dei PID — PID 1 nel container | Media |
| **Network** | `CLONE_NEWNET` | Interfacce di rete, routing, socket | Alta |
| **UTS** | `CLONE_NEWUTS` | hostname, domainname | Bassa |
| **IPC** | `CLONE_NEWIPC` | System V IPC, POSIX mq | Bassa |
| **User** | `CLONE_NEWUSER` | UID/GID mapping (root nel container → user fuori) | Alta |
| **Cgroup** | `CLONE_NEWCGROUP` | Vista dei cgroup | Alta (richiede M11-07f) |

---

#### M11-07a · Mount Namespace (`CLONE_NEWNS`)

Ogni processo con mount namespace proprio ha una **copia privata** della mount table.

- `unshare(CLONE_NEWNS)`: duplica la mount table corrente nel TCB → il processo
  ha la propria vista VFS indipendente
- `mount()` / `umount()` in un namespace non impatta gli altri — ogni entry nella
  mount table ha un `ns_id` che filtra la visibilità
- **Bind mount** già pianificato in M9-04 — necessario per costruire il rootfs container
- **Pivot root** già pianificato in M9-04 — necessario per cambiare root del container
- **OverlayFS** (vedi M11-07e) — necessario per image layering Docker

**Estensione `sched_tcb_t`:**
```c
uint32_t  mnt_ns_id;    /* 0 = namespace globale */
uint32_t  pid_ns_id;
uint32_t  net_ns_id;
uint32_t  uts_ns_id;
uint32_t  ipc_ns_id;
uint32_t  user_ns_id;
```

---

#### M11-07b · PID Namespace (`CLONE_NEWPID`)

Il primo processo nel namespace ha PID 1 nel namespace, ma un PID diverso nel namespace genitore.

- `getpid()` ritorna il PID nel namespace corrente del processo
- Il kernel mantiene due PID: `global_pid` (univoco) + `ns_pid` (locale al namespace)
- `kill(1, SIGTERM)` nel namespace → invia al PID 1 del namespace (non al PID 1 globale)
- Se il PID 1 del namespace muore → tutti i processi nel namespace ricevono SIGKILL
- `/proc/[pid]/status` mostra il PID locale al namespace

---

#### M11-07c · UTS Namespace (`CLONE_NEWUTS`)

Più semplice — ogni namespace ha il proprio `hostname` e `domainname`.

- `sethostname()` / `gethostname()` — syscall nr 170/171 — già o quasi già presenti
- `setdomainname()` / `getdomainname()` — nr 166/167
- Campo `char hostname[64]` + `char domainname[64]` nel namespace UTS
- Utile per container che si vedono come host separati

---

#### M11-07d · Network Namespace (`CLONE_NEWNET`)

Il più complesso: ogni namespace ha interfacce virtuali, routing table, socket propri.

- Ogni network namespace ha almeno un'interfaccia `lo` (loopback) interna
- **veth pair** (virtual ethernet): coppia di interfacce virtuali collegate — una dentro
  il container, una fuori (nel namespace host) — trafic passa da una all'altra
- Il server `netd` (M10-01) esteso per gestire network namespace multipli
- `ip link add veth0 type veth peer name veth1` — crea veth pair
- `ip link set veth1 netns <container_ns>` — sposta un'interfaccia in un altro namespace
- Routing isolato: ogni namespace ha la propria routing table (server `netd`)

---

#### M11-07e · OverlayFS

Necessario per Docker image layering: più layer read-only + un layer read-write in cima.

```
upper/  (read-write — modifiche del container)
lower/  (read-only — image layers)
merged/ (vista unificata — mountpoint del container)
work/   (directory temporanea richiesta da overlayfs)
```

- `mount("overlay", "/merged", "overlay", 0, "lowerdir=/lower,upperdir=/upper,workdir=/work")`
- Lettura: cerca in `upper`, se non trovato cerca in `lower`
- Scrittura: copy-on-write — copia il file da `lower` in `upper` al primo write
- `unlink` su file da `lower`: crea un **whiteout** (`char device 0:0`) in `upper`
- Implementato come backend VFS in `vfsd` (M9-02) — nessun codice kernel aggiuntivo

---

#### M11-07f · Cgroup v2

Control groups per limitare risorse (CPU, memoria, I/O) per gruppo di processi.
Base di Docker `--memory`, `--cpus`, `--device-read-bps`.

**Gerarchia cgroup:**
```
/sys/fs/cgroup/              ← mountpoint cgroup v2
    cpu.max                  ← quota CPU (es. "50000 100000" = 50% di un core)
    memory.max               ← limite memoria in byte
    memory.current           ← uso attuale
    io.max                   ← limite IOPS/bandwidth per device
    pids.max                 ← limite numero processi
    cgroup.procs             ← lista PID nel gruppo
```

**Integrazione con lo scheduler (M2-03 / M13-01):**
- `cpu.max`: il kernel controlla il budget CPU del gruppo a ogni tick; se il gruppo
  ha esaurito il quota → i suoi task non vengono schedulati fino al prossimo periodo
- `memory.max`: `phys_alloc_page()` controlla il contatore del cgroup; se supera
  il limite → `ENOMEM` o OOM killer (uccide il processo con più memoria nel gruppo)
- `pids.max`: `fork()` controlla il contatore del cgroup prima di creare il processo

**Struttura file:**
```
kernel/namespace.c      — clone flags NEWNS/NEWPID/NEWUTS/NEWNET/NEWIPC/NEWUSER
kernel/cgroup.c         — gerarchia cgroup v2, controller CPU/memory/pids
drivers/overlay.c       — OverlayFS backend per vfsd
include/namespace.h
include/cgroup.h
```

**Sblocca:** Docker/Podman AArch64 nativo, LXC, sandbox applicazioni, isolamento
servizi (ogni server di sistema in un namespace separato — security hardening EnlilOS)

---

## Dipendenze aggiornate (M11-06 e M11-07)

> **Principio:** il server grafico è un processo user-space a priorità media-alta.
> Il compositor opera su un deadline vsync (16.67ms a 60Hz, 6.94ms a 144Hz).
> I client (app) scrivono in surface buffer GPU condivisi e notificano il compositor
> via IPC. Il compositor fa page-flip atomico tramite le syscall GPU di M3-04.
> Nessun pixel viene composto dalla CPU: tutto passa per la GPU (M5b-04).

---

### ⬜ M12-01 · Wayland Protocol Server Minimale (`wld`)
**Priorità:** ALTA

Implementazione del sottoinsieme Wayland strettamente necessario per applicazioni grafiche.

**Protocolli supportati inizialmente:**
- `wl_compositor` — crea superfici
- `wl_surface` — attach buffer, commit, damage
- `wl_shm` — buffer condiviso CPU-accessible (per app senza GPU diretto)
- `xdg_wm_base` / `xdg_surface` / `xdg_toplevel` — finestre
- `wl_seat` — input (keyboard + pointer) dal server `inputd`
- `wl_output` — informazioni display (risoluzione, refresh, scala)

**Trasporto:** socket Unix (`/run/wayland-0`) via AF_UNIX + capability buffer condiviso.
I messaggi Wayland sono piccoli (< 64 byte quasi sempre) → zero-copy via IPC EnlilOS.

**Server `wld`:** processo user-space, priorità 16 (appena sotto i task hard-RT).
Loop principale sincrono al vsync IRQ (M5b-02): ogni 16.67ms raccoglie i commit
delle superfici e invia il frame finale al display engine.

---

### ⬜ M12-02 · Window Manager RT (`wm`)
**Priorità:** MEDIA (dipende da M12-01)

Window manager separato dal compositor (Wayland standard: WM è un client speciale).

- Decorazioni server-side: barra titolo, bordi, ombra (renderizzati via M5b-04)
- Tiling layout: suddivisione schermo in colonne/righe (stile `sway`/`i3`)
- Focus: click-to-focus, segue il puntatore opzionale
- Animazioni: fade-in/out con alpha blend GPU — bounded latency, massimo 8 frame
- Shortcut globali: `SUPER+ENTER` apre terminale, `SUPER+Q` chiude finestra
- Comunicazione con `wld` via protocollo `xdg_wm_base` + estensione privata `enlil_wm_v1`

---

### ⬜ M12-03 · GPU Shader Pipeline per Compositor
**Priorità:** MEDIA (dipende da M5b-04 e M12-01)

Passaggio da operazioni 2D CPU-assisted a compute shader GPU nativi per il compositing.

- Shader di compositing: `alpha_blend.comp` — GLSL compute compilato offline in binario AGX
- `gpu_compute_dispatch()` (M3-04 nr 133) per ogni frame del compositor
- Texture: surface client come `GPU_BUF_TEXTURE`, scanout come `GPU_BUF_SCANOUT`
- Round-trip: `gpu_cmdbuf_begin → draw surfaces → alpha blend → gpu_present → gpu_fence_wait`
- Su QEMU: software fallback già implementato in M5b-04
- Su Apple M-series: shader AGX compilati da `anecc` o `metal-shaderconverter`

---

## MILESTONE 13 — Scheduler Avanzato e SMP

### ⬜ M13-01 · EDF Scheduler (Earliest Deadline First)
**Priorità:** MEDIA (alternativa configurabile al FPP di M2-03)

- Rinomina `deadline_ms` → `deadline_abs_ns` nel TCB (già a offset 40, stessa width uint64_t — nessun impatto su `sched_switch.S`)
- Run queue EDF: min-heap a 32 entry (SCHED_MAX_TASKS) con `deadline_abs_ns` come chiave — O(log 32) = 5 confronti
- `sched_pick_next_edf()`: estrae il task con deadline minima — costo equivalente al `bitmap_find_first()` FPP
- Schedulabilità: test RMS (`sum(Ci/Ti) ≤ 1`) al momento dell'ammissione via `sched_task_set_rt(period_ns, wcet_ns)`
- Modalità configurabile a boot: `SCHED_MODE_FPP` (default) / `SCHED_MODE_EDF` / `SCHED_MODE_FPP_NAS` / `SCHED_MODE_EDF_NAS`
- CBS (Constant Bandwidth Server): task aperiodici (blkd, vfsd, NSH) ottengono budget Cs ogni periodo Ts; deadline rinnovata a ogni esaurimento
- Tie-breaking su deadline uguale: PID crescente (deterministico)
- Selftest `edf-core`: 3 task periodici con WCET noto, verifica zero miss in 1 secondo

**Prerequisiti:** M2-03 (già completato)  
**Dipende da:** nessun altro — può iniziare subito dopo M13-02

**Riferimento architetturale:** `docs/STUDIO_SCHEDULER_EDF_NAS.md`

---

### ⬜ M13-04 · Neural-Assisted Scheduler — Profiler e FPP Advisor
**Priorità:** MEDIA (lavoro parallelo a M13-01 / M13-03)

Primo stadio del NAS (Neural-Assisted Scheduler): profiler lockless + advisor su FPP.
Il NAS è un layer **adattivo** che opera fuori dal fast path: osserva il comportamento
dei task e suggerisce aggiustamenti via `sched_task_donate_priority()`.
Il kernel rimane deterministico; il NPU è un advisor, non un controller.

**M13-04a — Profiler lockless (`kernel/sched_prof.c`)**

- Ring buffer lock-free `prof_buf[256]` scritto in `sched_tick()` (O(1), 1 store atomico, no lock)
- Per task: `runtime_ns`, `block_count`, `ipc_calls`, `miss_deadline`, `avg_wake_latency_us`
- `prof_drain()`: API per il NAS task che restituisce snapshot aggregato su N tick
- Overhead: < 0.1% CPU (misurabile con M13-03 WCET framework)

**M13-04b — Feature extractor e NAS task**

- NAS task kernel a `PRIO_LOW=200`, loop 100ms — mai nel fast path IRQ
- Feature vector 16 × float16 per task (32 task = 512 float16 = 1 KB):
  `runtime_ratio`, `block_rate`, `ipc_rate`, `wake_latency_us`, `miss_deadline_rate`,
  `priority_norm`, `donated_prio_norm`, `flags_onehot`, `runtime_delta`, history T-1/T-2/T-3
- Batch inference su ANE (DMA async, `ane_submit_inference()` + `ane_wait()`)
- SW fallback NEON se ANE non disponibile

**M13-04c — Modello TCN embedded**

- Temporal Convolutional Network leggera (~8K parametri float16 = 16 KB)
- Architettura: `Conv1D(k=3,f=32,d=1) → Conv1D(k=3,f=32,d=2) → LayerNorm → Linear(32→16) → Linear(16→4)`
- Output per task: `delta_prio int8[-32,+32]`, `delta_quantum int8[-10,+10]`, `delta_budget int8[-5,+5]`, `confidence uint8`
- Pesi embedded come array C (generati offline da training pipeline su trace QEMU)

**M13-04d — Guardrail layer**

- `TCB_FLAG_RT`: delta_prio ≥ 0 sempre scartato (task RT mai degradati)
- `confidence < NAS_CONFIDENCE_MIN (128)`: suggerimento scartato
- Donation resettata ogni 500ms se confidence rimane bassa (anti-starvation)
- Applica via `sched_task_donate_priority()` — mai mutazione diretta `task->priority`
- Selftest `nas-core`: verifica profiler drain, confidence gate, zero degradazione task FLAG_RT

**Prerequisiti:** M2-03, M3-01 (syscall base), ANE stub (M3-03, già presente)  
**Dipende da:** nessun altro — può procedere in parallelo a M13-01

**Riferimento architetturale:** `docs/STUDIO_SCHEDULER_EDF_NAS.md`

---

### ⬜ M13-05 · Neural-Assisted Scheduler — Integrazione EDF
**Priorità:** BASSA (dipende da M13-01 + M13-04)

Secondo stadio del NAS: estende l'advisor per operare sui parametri EDF.
Risolve il problema principale di EDF puro: la stima manuale di WCET (`Ci`).

- **WCET adattivo**: EMA 95° percentile di `runtime_ns` → aggiorna `wcet_estimate_ns` per il test di ammissione CBS
- **`deadline_slack_ns`**: il NAS può stringere/allargare la deadline percepita dall'heap EDF
  (`deadline_eff = deadline_abs_ns - slack_ns`) senza mai posticipare la deadline reale
- **CBS budget adattivo**: il NAS regola il budget Cs dei task aperiodici (blkd, vfsd, NSH) in base al carico osservato
- Hard RT protetti: task `FLAG_RT` con deadline dichiarata esplicitamente → NAS solo osserva, non tocca
- Modalità risultante: `SCHED_MODE_EDF_NAS` — scheduler ibrido deterministico + adattivo

**Separazione formale delle responsabilità:**

```
Dominio EDF (hard RT, certificabile):
  task FLAG_RT → deadline fissa, WCET dichiarato, test RMS, NAS bloccato da guardrail

Dominio NAS (soft-RT e aperiodici, best-effort):
  task user non-RT → WCET adattivo, CBS budget dinamico, deadline_slack suggerita dal NAS
```

**Prerequisiti:** M13-01 (EDF), M13-04 (NAS profiler + modello)

**Riferimento architetturale:** `docs/STUDIO_SCHEDULER_EDF_NAS.md`

---

### ⬜ M13-02 · Multi-Core SMP (AArch64)
**Priorità:** ALTA — QEMU virt ha 4 core disponibili

- **Spin-up core secondari** via `PSCI_CPU_ON` (PSCI 1.0 su QEMU):
  core 1/2/3 entrano in `secondary_entry` in `vectors.S`, inizializzano MMU e GIC locali
- **Percpu data**: `TPIDR_EL1` punta a `per_cpu_t { uint32_t cpu_id; sched_tcb_t *current; ... }`
- **Run queue per-core**: ogni core ha la propria ready bitmap + run_queue[256] (M2-03 replicato)
- **Load balancer**: ogni 10ms, il core idle con più task ha overbooking → migrazione singolo task
- **Spinlock IPC-safe**: `spinlock_t` con `LDAXR/STLXR` (AArch64 exclusives); mai acquisiti con IRQ abilitati per più di 50 cicli
- **IPI (Inter-Processor Interrupt)**: GIC SGI (Software Generated Interrupt) per reschedule remoto
  - SGI #0: `IPI_RESCHED` — il core target ri-valuta `need_resched` alla prossima uscita da IRQ
  - SGI #1: `IPI_CALL` — esegue una funzione su un core specifico (TLB invalidation)
  - SGI #2: `IPI_STOP` — ferma un core (per panic o poweroff)
- **TLB shootdown**: `tlb_flush_range(vaddr, size)` invia IPI a tutti i core prima di `TLBI`

**Invarianti RT su SMP:**
- Un task hard-RT è pinnato su un core dedicato (`TASK_PINNED_CPU`) — mai migrato
- Le run queue per-core sono indipendenti: il balancer non tocca mai la core RT
- Spinlock nei path critici: WCET spinlock bounded a `MAX_SPIN_CYCLES = 200`; se supera → panic

---

### ⬜ M13-03 · WCET Measurement Framework
**Priorità:** MEDIA

- Macro `WCET_BEGIN(label)` / `WCET_END(label)`: legge `PMCCNTR_EL0` (CPU cycle counter)
- Tabella statica `wcet_table[MAX_LABELS]`: min, max, sum, count per ogni label
- Syscall `wcet_query(label, wcet_stat_t *out)` per lettura da user-space
- Export via `/proc/wcet` (estensione del `procfs` di M14-01)
- Alert: se `max_cycles > WCET_THRESHOLD(label)` → log + contatore overflow
- PMU (Performance Monitor Unit): abilita `PMCR_EL0`, conta cicli e istruzioni
- Instrumentazione automatica opzionale: wrapper attorno alle syscall RT per misurare
  latenza kernel end-to-end

---

## MILESTONE 14 — Utilities di Sistema

### ✅ M14-01 · procfs / sysfs (procfs core v1)
**Priorità:** MEDIA

Filesystem virtuale per ispezione dello stato kernel da user-space.

**Stato attuale:** implementato `procfs` core read-only, montato su `/proc`,
navigabile da shell e coperto dal selftest `procfs-core`. La parte `sysfs`
e la futura migrazione a un server `procfsd` restano lavoro successivo.

- `/proc/` — directory root
- `/proc/sched` — snapshot scheduler con `jiffies` e task presenti
- `/proc/<pid>/status` — stato task: nome, pid, priorita', runtime, stato
- `/proc/self` e `/proc/self/status` — alias del task corrente
- mount automatico in `vfs_init()`
- backend read-only in-kernel con `vfs_ops_t` dedicato
- selftest automatico `procfs-core` nel run completo `SUMMARY total=25 pass=25 fail=0`

**Resta da completare:**
- `/proc/<pid>/maps`
- `/proc/<pid>/fd`
- `/proc/wcet`
- export `/sys/*`
- eventuale migrazione verso `procfsd` / `sysfsd` user-space sopra `vfsd`

---

### ✅ M14-02 · Crash Reporter e Kernel Debugger
**Priorità:** ALTA (fondamentale durante lo sviluppo)

- **Kernel panic handler** migliorato: dumpa stack trace simbolico via tabella ELF `.symtab`
  generata a build-time e inclusa nel binario `enlil.elf`
- **Stack unwinder AArch64**: segue frame pointer (`x29/x30`) — richiede `-fno-omit-frame-pointer`
- **KGDB stub** (opzionale): rinviato; non blocca la milestone, il crash reporter è già autosufficiente
- **Assertion RT-safe**: `KASSERT(cond)` → stampa file:line + valore registri + panic; nessuna stringa
  dinamica allocata nell'handler
- **Memory corruption detector**: canary a 0xDEADC0DE sui bordi degli slab; check a ogni `kfree()`
- **Task watchdog**: timer hardware secondario con campionamento 5ms e timeout >10ms sul progresso del tick;
  se il tick non avanza → dump + panic
- **Self-test**: `kdebug-core` copre lookup simboli, unwinder base e attivazione watchdog

---

### ⬜ M14-03 · Power Management (PSCI)
**Priorità:** BASSA

- `poweroff()` via PSCI `SYSTEM_OFF` (nr 0x84000008)
- `reboot()` via PSCI `SYSTEM_RESET` (nr 0x84000009)
- `cpu_suspend()` per mettere core secondari in low-power (M13-02)
- Syscall nr 169 `reboot(magic, cmd)` — solo da processo privilegiato
- Clock gating: `cpufreq` stub (no DVFS reale su QEMU; hook per M-series)

---

### ⬜ M14-04 · VMware Fusion Image Builder (ARM64 / UEFI)
**Priorità:** MEDIA

Costruzione di un'immagine avviabile su **VMware Fusion ARM64** con output pronto
per distribuzione e test, senza dipendere da QEMU come runtime finale.

- Target build dedicato: `make vmware-image`
- Output minimi:
  - disco bootabile `enlilos-vmware.vmdk`
  - configurazione VM `enlilos-arm64.vmx`
  - metadati esportabili `OVF`/`OVA` opzionali
- Boot path UEFI standard ARM64:
  - GPT con EFI System Partition FAT32
  - loader in `/EFI/BOOT/BOOTAA64.EFI`
  - kernel + initrd o rootfs ext4 secondo profilo immagine
- Due profili iniziali:
  - `debug`: seriale attiva, initrd minimale, tool di diagnostica
  - `full`: rootfs ext4 persistente, shell di default, config base sistema
- Toolchain immagine host:
  - generazione raw/GPT
  - conversione raw → VMDK
  - template `.vmx` versionato nel repo
  - checksum e manifest dell'artefatto
- Supporto device minimo richiesto lato guest per il primo boot su VMware:
  - UEFI GOP per framebuffer iniziale
  - storage VMware compatibile (`NVMe` preferito, `AHCI/SATA` come fallback)
  - seriale virtuale per debug e recovery
- Configurazione VM consigliata:
  - 2-4 vCPU
  - 1-2 GB RAM
  - firmware UEFI
  - disco primario VMDK
- Test di accettazione:
  - boot fino alla shell/login senza intervento manuale
  - mount del rootfs
  - input tastiera funzionante
  - poweroff/reboot puliti da guest
- CI/release:
  - job separato che produce l'immagine VMware dagli artifact kernel/rootfs
  - naming versionato `enlilos-<version>-arm64-vmware.ova`

**Nota architetturale:** questa milestone copre il packaging e il boot profile VMware.
L'eventuale supporto a device VMware-specifici aggiuntivi (`vmxnet3`, SVGA avanzata,
guest tools) resta in milestone dedicate se necessario.

#### M14-04a · Driver Storage VMware Compatibile (`NVMe`, fallback `AHCI/SATA`)

Perché l'immagine VMware sia davvero bootabile in modo affidabile, serve un path
storage nativo compatibile con i controller più realistici esposti da VMware Fusion ARM64.

- Driver blocchi per controller **NVMe** come target principale di boot
- Fallback **AHCI/SATA** per immagini o profili VM meno moderni
- Discovery controller via PCI/UEFI enumeration, non hardcoded
- Integrazione con il block layer esistente:
  - espone lo stesso contratto di `virtio-blk`
  - mount rootfs via VFS senza distinguere il backend nel livello superiore
- Supporto minimo richiesto:
  - identify namespace/device
  - read path stabile per boot
  - write path sufficiente a rootfs ext4 persistente
  - flush/sync per `fsync`, `sync`, shutdown pulito
- Obiettivo primo boot:
  - EFI System Partition leggibile
  - kernel e initrd caricabili
  - rootfs ext4 su disco VMware montabile in rw
- Test matrix minima:
  - VMware Fusion ARM64 + NVMe virtual disk
  - VMware Fusion ARM64 + SATA/AHCI virtual disk
  - cold boot, reboot, poweroff, remount rw, fsync

**Scelta progettuale consigliata:** sviluppare prima `NVMe`, poi aggiungere `AHCI`
solo come fallback di compatibilità e recovery.

**Dipende da:** M5-03, M5-04, M9-03
**Sblocca:** boot reale da disco VMware, persistenza su VMware Fusion, immagini demo
e sviluppo non più vincolato a `virtio-blk`

**Dipende da:** M5-03, M5-04, M14-03
**Sblocca:** test su VMware Fusion, distribuzione a utenti Mac ARM, immagini demo/release

---

## MILESTONE 15 — Audio

> **Principio:** il sottosistema audio è un server user-space (`audiod`) a priorità
> alta ma non hard-RT. Il buffer audio ha dimensione fissa e viene riempito da
> un mixing thread con deadline periodica (es. 5ms a 48kHz con buffer 256 sample).
> I task hard-RT non chiamano mai API audio direttamente — depositano campioni in
> un ring buffer SPSC e lasciano che `audiod` li consumi.

---

### ⬜ M15-01 · Driver Audio VirtIO (virtio-sound)
**Priorità:** ALTA

Hardware target: QEMU `-device virtio-sound-pci` (spec VirtIO 1.2, device ID 25).

**RT design:** il driver produce/consuma blocchi PCM di dimensione fissa senza
allocazione dinamica. Latenza output = dimensione buffer / sample rate.

- Probe virtio-mmio / virtio-pci: device ID 25, feature negotiation
  (`VIRTIO_SND_F_CTLS` per controllo volume)
- 4 vrings: `controlq`, `eventq`, `txq` (playback CPU→device), `rxq` (capture device→CPU)
- Negoziazione formato al boot: 48000 Hz, stereo, S16LE (poi S32LE / F32LE se disponibile)
- `txq`: pool di descriptor pre-allocati con buffer PCM da 256 sample (= 5.33ms a 48kHz)
  Kick virtio dopo ogni descriptor → latenza costante
- `rxq`: descriptor sempre in coda per cattura; IRQ RX → dato in ring buffer SPSC
- IRQ: `GIC_PRIO_DRIVER` (0x80) — non interrompe task hard-RT
- API kernel interna:
  - `audio_play_block(pcm_buf, n_samples)` — deposita blocco in txq; bloccante se coda piena
  - `audio_capture_block(pcm_buf, n_samples) → int` — preleva da rxq; 0 se vuoto

**Strutture:**
```c
typedef struct {
    uint32_t  sample_rate;   /* 44100 / 48000 / 96000 */
    uint8_t   channels;      /* 1=mono, 2=stereo */
    uint8_t   format;        /* AUDIO_FMT_S16 / S32 / F32 */
    uint16_t  block_size;    /* campioni per blocco (es. 256) */
} audio_caps_t;

typedef struct {
    int16_t   samples[];     /* interleaved: L0 R0 L1 R1 ... */
} audio_block_t;
```

**Target QEMU:** `run-audio` in Makefile con `-device virtio-sound-pci,audiodev=pa0`
e `-audiodev pa,id=pa0` (PulseAudio host) oppure `-audiodev alsa,id=pa0`.

---

### ⬜ M15-02 · Driver Audio PL041 (ARM AACI / AC97)
**Priorità:** MEDIA (hardware legacy su QEMU `virt` senza virtio-sound)

PL041 AACI (Advanced Audio CODEC Interface) @ `0x10004000` su QEMU `versatilepb`.
Alternativa per ambienti QEMU senza virtio-sound.

- MMIO: `AACI_DR` (data register), `AACI_SR` (status), `AACI_RXCR`/`TXCR`
- Canale AC97: 48kHz stereo S16LE — non configurabile su PL041
- IRQ #24 per TX empty (slot libero nel FIFO 8-entry)
- DMA: PL041 non supporta DMA → CPU scrive sample a sample nel FIFO in ISR
- Latenza peggiore = 8 sample / 48000Hz ≈ 167µs → accettabile per non-RT audio

---

### ⬜ M15-03 · Audio Server (`audiod`)
**Priorità:** ALTA (dipende da M15-01 o M15-02)

Server user-space a priorità 12 (alta, ma preemptibile da task RT).

**Architettura:**
```
Client 1 (app) → ring SPSC 4096 sample → ─┐
Client 2 (app) → ring SPSC 4096 sample → ─┤→ audiod mixer → driver audio
Client N (app) → ring SPSC 4096 sample → ─┘   (256 sample / 5ms)
```

**Loop principale (`audiod`):**
1. Ogni 5ms (timer `SIGALRM` via M8-03 o `nanosleep`):
   - Legge fino a 256 sample da ogni client ring SPSC
   - **Mixing software**: somma campioni S32 (overflow headroom), clip a S16
   - Deposita il blocco misto nel driver (`audio_play_block`)
2. Se un client ring è vuoto → riempie con silenzio (zero-fill) — no glitch

**IPC client ↔ audiod:**
- Socket Unix `/run/audio` (M10-03 AF_UNIX) oppure porta IPC EnlilOS (M7-01)
- Messaggi di controllo (< 64 byte): `AUDIO_OPEN`, `AUDIO_CLOSE`, `AUDIO_SET_VOLUME`,
  `AUDIO_SET_FORMAT`, `AUDIO_PAUSE`, `AUDIO_RESUME`
- Trasferimento audio: capability su buffer SPSC shared memory (M9-01) — zero-copy

**Gestione volume per client:**
- Volume 0–255 per stream, volume master globale
- Applicato in fixed-point Q8.8 durante il mixing

**Latenza end-to-end:**
- App scrive campioni → ring SPSC: O(1) senza blocco
- `audiod` svuota ring ogni 5ms → driver → hardware: 5ms
- Totale peggiore: ~10ms (2 tick audio) — adeguato per tutto eccetto hard-RT audio

---

### ⬜ M15-04 · Syscall Audio
**Priorità:** ALTA (dipende da M15-03)

**Numeri syscall (range dedicato 140–159):**

| Nr  | Nome                  | RT-safe | Firma C                                                   | Note |
|-----|-----------------------|---------|-----------------------------------------------------------|------|
| 140 | `audio_open`          | No      | `(audio_fmt_t *fmt) → audio_stream_t`                    | Apre stream; negozia formato con `audiod` |
| 141 | `audio_close`         | Sì — O(1) | `(audio_stream_t s)`                                   | Chiude stream, libera slot |
| 142 | `audio_write`         | **Sì**  | `(audio_stream_t s, const void *pcm, size_t n_samples) → int` | Deposita campioni nel ring SPSC; non-blocking se spazio disponibile |
| 143 | `audio_read`          | **Sì**  | `(audio_stream_t s, void *pcm, size_t n_samples) → int`  | Cattura microfono; EAGAIN se vuoto |
| 144 | `audio_set_volume`    | Sì — O(1) | `(audio_stream_t s, uint8_t vol)`                      | Volume 0–255 per stream |
| 145 | `audio_get_latency`   | Sì — O(1) | `(audio_stream_t s) → uint32_t`                        | Ritorna latenza stimata in µs |
| 146 | `audio_query_caps`    | Sì — O(1) | `(audio_caps_t *out)`                                  | Formato hw supportato, sample rate, canali |
| 147 | `audio_pause`         | Sì — O(1) | `(audio_stream_t s)`                                   | Sospende stream senza chiuderlo |
| 148 | `audio_resume`        | Sì — O(1) | `(audio_stream_t s)`                                   | Riprende stream |

**RT constraint su `audio_write`:** se il ring SPSC è pieno → ritorna `EAGAIN` immediatamente
(mai blocca un task hard-RT). Il caller è responsabile di non overflow del ring.

---

### ⬜ M15-05 · Codec e DSP Software
**Priorità:** BASSA (dipende da M15-03)

Pipeline di elaborazione audio lato `audiod` o lato client:

- **Resampling**: sample rate conversion via filtro polifase FIR a 32 tap (NEON AArch64)
  — necessario se il client produce a 44100Hz e l'hardware vuole 48000Hz
- **Formato conversion**: S16LE ↔ S32LE ↔ F32LE — tutto via NEON `vcvtq`
- **Equalizzatore parametrico**: 5 bande, filtri biquad in cascade; coefficienti aggiornabili
  a runtime da `audiod` senza interrompere il playback
- **Compressore dinamico**: attenuazione automatica dei picchi → evita clipping
  sull'output hardware
- **OPUS codec** (porta): `libopus` AArch64 per stream audio compressi via rete (M10-02).
  Decompressione: ~1.5ms per frame 20ms a 48kHz → budget CPU accettabile

---

## MILESTONE 16 — USB

> **Principio:** USB è gestito da un server user-space (`usbd`) con accesso MMIO
> privilegiato al controller tramite capability `CAP_MMIO` (M9-01). I driver di
> classe (HID, MSC, CDC) sono plug-in del server. Il kernel non contiene codice USB.
> Su QEMU si usa XHCI (`-device qemu-xhci`) che implementa USB 3.x.

---

### ⬜ M16-01 · Driver XHCI (Host Controller)
**Priorità:** ALTA

XHCI (eXtensible Host Controller Interface) è lo standard USB 3.x:
QEMU `-device qemu-xhci` lo espone come PCI device (class 0x0C 0x03 0x30).

**Inizializzazione:**
- Detect PCI: BDF scan, vendor/class check, BAR0 mmap via capability `CAP_MMIO`
- Reset controller: `USBCMD.HCRST` → attende `USBSTS.CNR == 0`
- Configura `DCBAAP` (Device Context Base Array): array di 256 puntatori a device context
- Alloca command ring (TRBS statici × 64), event ring (TRB statici × 256)
- Configura `ERSTBA` (Event Ring Segment Table Base Address)
- Abilita IRQ MSI (Message Signaled Interrupt) via APIC/GIC

**Command ring (produttore CPU):**
- `ENABLE_SLOT` → ottiene slot ID per nuovo device
- `ADDRESS_DEVICE` → assegna indirizzo USB e carica Device Context
- `CONFIGURE_ENDPOINT` → configura endpoint IN/OUT per il device
- `EVALUATE_CONTEXT` → aggiorna parametri endpoint (max packet size dopo get_descriptor)
- Ogni comando aspetta Event TRB di completamento via fence

**Transfer ring per endpoint:**
- Un transfer ring statico per endpoint (max 8 endpoint per device)
- TRBs: `NORMAL` (bulk/interrupt), `SETUP`/`DATA`/`STATUS` (control)
- Completamento segnalato via event ring IRQ → `usbd` gestisce il completamento

**Strutture XHCI (conformi alla spec):**
```c
typedef struct {               /* Transfer Request Block */
    uint64_t  parameter;
    uint32_t  status;
    uint32_t  control;         /* TRB type, cycle bit, flags */
} xhci_trb_t;

typedef struct {               /* Device Context (in-memory) */
    uint32_t  slot_ctx[8];     /* stato device: indirizzo, velocità, hub info */
    uint32_t  ep_ctx[2][8];    /* stato endpoint 0..15 (in/out) */
} xhci_device_ctx_t;
```

**Enumerate device al plug:** Port Status Change Event → `usbd` esegue la procedura
di enumerazione (reset porta, `ENABLE_SLOT`, `ADDRESS_DEVICE`, `GET_DESCRIPTOR`)

**Dipende da:** M9-01 (capability MMIO), M9-02 (vfsd — opzionale per mount MSC)

---

### ⬜ M16-02 · USB Hub e Port Management
**Priorità:** MEDIA (dipende da M16-01)

- Gestione porte root hub (tipicamente 4 porte su XHCI QEMU)
- Supporto external hub USB 2.0/3.0: `SET_ADDRESS` + `GET_DESCRIPTOR` ricorsivo
- Hot-plug IRQ: Port Status Change Event → `usbd` richiama enumerate + carica driver classe
- Hot-unplug: `DISABLE_SLOT` + dealloca TRB rings + notifica driver classe → cleanup
- Speed negotiation: LS (1.5Mbps) / FS (12Mbps) / HS (480Mbps) / SS (5Gbps)

---

### ⬜ M16-03 · Classe HID — Tastiera e Mouse USB
**Priorità:** ALTA (dipende da M16-01)

USB HID (Human Interface Device, classe 0x03) per tastiera e mouse plug-in.

**Protocol:**
- Endpoint interrupt IN da 8 byte ogni 1–10ms (polling interval dal descriptor)
- HID Boot Protocol: tastiera report = `[modifier, reserved, key1, key2, ..., key6]`
- HID Boot Protocol: mouse report = `[buttons, dx, dy, dwheel]`

**Integrazione con il subsistema input (M4-01..M4-02):**
- Il driver HID converte i report HID → eventi `key_event_t` / `mouse_event_t` standard
- Deposita negli stessi ring SPSC già usati da VirtIO input (M4-02)
- `keyboard_getc()` e `mouse_get_event()` rimangono invariate — l'origine è trasparente
- Supporto layout tastiera: scancode set 2 → keycode → ASCII (tabella esistente M4-01)
- LED feedback: `SET_REPORT` per Caps Lock / Num Lock / Scroll Lock

**Strutture:**
```c
typedef struct {
    uint8_t  modifier;    /* bit: CTRL-L, SHIFT-L, ALT-L, META-L, ... */
    uint8_t  reserved;
    uint8_t  keycode[6];  /* fino a 6 tasti simultanei */
} hid_kbd_report_t;

typedef struct {
    uint8_t  buttons;     /* bit 0=L, 1=R, 2=M */
    int8_t   dx, dy;      /* spostamento relativo */
    int8_t   wheel;
} hid_mouse_report_t;
```

---

### ⬜ M16-04 · Classe MSC — Mass Storage (USB Flash Drive)
**Priorità:** ALTA (dipende da M16-01)

USB MSC (Mass Storage Class, classe 0x08) per chiavette USB e dischi esterni.

**Subclass/Protocol:** SCSI Transparent (subclass 0x06) + Bulk-Only Transport (protocol 0x50).

**Bulk-Only Transport:**
- CBW (Command Block Wrapper): 31 byte, contiene CDB SCSI
- Data phase: trasferimento dati su endpoint bulk IN/OUT
- CSW (Command Status Wrapper): 13 byte, stato del comando

**Comandi SCSI implementati:**
- `INQUIRY` (0x12) — identificazione device (vendor, product, revision)
- `TEST_UNIT_READY` (0x00) — verifica se pronto
- `READ_CAPACITY(10)` (0x25) — dimensione disco in settori
- `READ(10)` (0x28) — legge N settori da LBA
- `WRITE(10)` (0x2A) — scrive N settori a LBA
- `REQUEST_SENSE` (0x03) — recupera errore esteso dopo failure

**Integrazione con VFS:**
- Il driver MSC espone la stessa API del driver `blk` di M5-01:
  `blk_read_sync(sector, buf, count)` / `blk_write_sync(sector, buf, count)`
- `usbd` notifica `vfsd` all'enumerazione: `vfs_mount("/usb0", blk_msc_ops, "auto")`
  → `vfsd` prova mount ext4 o FAT32 (M16-05) automaticamente
- Hot-unplug: `vfsd` riceve notifica → `umount("/usb0")` + sync dirty pages prima

**Velocità attesa su QEMU:**
- USB 3.0 SuperSpeed (5Gbps) → `qemu-xhci` + chiavetta emulata → ~400MB/s teorici;
  in pratica limitato dall'emulazione: ~50–100MB/s — accettabile per storage secondario

---

### ⬜ M16-05 · FAT32 — Mount chiavette USB
**Priorità:** MEDIA (dipende da M16-04)

Le chiavette USB sono quasi sempre formattate FAT32 (o exFAT). ext4 su USB è raro.

**FAT32 read-only iniziale:**
- Boot sector: `BPB_BytsPerSec`, `BPB_SecPerClus`, `BPB_RootClus`
- FAT table: cluster chain per file (lookup O(N) sul path; cache cluster table in RAM)
- Directory entries: 32 byte, supporto LFN (Long File Name, UTF-16LE)
- `open`, `read`, `readdir`, `stat` mappati sul server VFS

**FAT32 write (seconda fase):**
- `write`, `mkdir`, `unlink`, `rename`
- Aggiornamento FAT su due copie (FAT1 e FAT2) — integrità in caso di unplug
- `fsync()` su MSC flush la write cache del device (`SYNCHRONIZE_CACHE` SCSI 0x35)

**exFAT (opzionale):**
- Supportato da SDXC e chiavette > 32GB
- Parsing cluster bitmap, directory entry `STREAM_EXTENSION` + `FILE_NAME`
- Solo read inizialmente — write più complessa (bitmap + checksum obbligatori)

---

### ⬜ M16-06 · Classe CDC-ACM — USB Seriale
**Priorità:** MEDIA (dipende da M16-01)

CDC-ACM (Communications Device Class — Abstract Control Model, classe 0x02/0x0A)
per dispositivi seriali USB: Arduino, convertitori CH340/FTDI, modem.

- Due endpoint: bulk IN (RX dal device), bulk OUT (TX verso device)
- Control endpoint: `SET_LINE_CODING` (baud, parità, stop bit), `SET_CONTROL_LINE_STATE`
- Ring buffer SPSC per RX/TX (stesso pattern di M4-01)
- Esposto come `/dev/ttyUSB0`, `/dev/ttyUSB1`, ...
- API: stessa di UART PL011 — `read(fd, buf, n)` / `write(fd, buf, n)` — trasparente per l'utente

**Casi d'uso su EnlilOS:**
- Debug di microcontroller da shell `nsh`
- Comunicazione con hardware embedded
- Modem PPP per connettività alternativa alla rete virtio

---

### ⬜ M16-07 · Classe UVC — USB Webcam
**Priorità:** BASSA (dipende da M16-01)

USB Video Class (UVC, classe 0x0E) per webcam.

- Probe/commit: negozia risoluzione (640×480 o 1280×720), formato (MJPEG o YUY2), fps
- Streaming interface: endpoint isocrono o bulk IN con payload video
- Frame buffer: ogni frame USB → decodifica MJPEG via CPU (libjpeg-turbo NEON) →
  buffer RGBA → disponibile come texture per EnlilGFX (BACKLOG3) o Wayland surface (M12-01)
- API kernel non bloccante: `uvc_get_frame(dev, buf, timeout_ns) → int`
- Limitazione XHCI QEMU: endpoint isocrono non supportato → necessario bulk fallback o
  passthrough device reale (`-device usb-host` in QEMU)

---

### ⬜ M16-08 · `usbd` — USB Daemon e Driver Manager
**Priorità:** ALTA (dipende da M16-01, coordina M16-02..M16-07)

Server user-space che centralizza tutta la gestione USB.

**Struttura interna:**
```
usbd
 ├── xhci_driver.c     — accesso diretto MMIO via CAP_MMIO
 ├── hub_manager.c     — enumera device, gestisce hot-plug/unplug
 ├── class_hid.c       — driver HID (M16-03)
 ├── class_msc.c       — driver MSC (M16-04)
 ├── class_cdc.c       — driver CDC-ACM (M16-06)
 ├── class_uvc.c       — driver UVC (M16-07)
 └── plugin_api.h      — interfaccia per driver classe custom
```

**Plugin API** per driver classe di terze parti:
```c
typedef struct {
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    int    (*probe) (usb_device_t *dev);    /* ritorna 0 se supportato */
    int    (*attach)(usb_device_t *dev);    /* inizializza il driver */
    void   (*detach)(usb_device_t *dev);    /* cleanup su unplug */
} usb_class_driver_t;
```

**Priorità `usbd`:** 20 (alta non-RT). L'enumerazione è lenta (centinaia di ms) ma
avviene una sola volta; il trasferimento dati in corso usa IRQ + ring SPSC.

**Interazione con VFS:**
- All'attach MSC: invia a `vfsd` `vfs_mount_req_t` con ops block MSC
- All'detach MSC: invia `vfs_umount_req_t` + attende flush dirty pages
- All'attach CDC: crea `/dev/ttyUSBN` via devfs (M9-02)
- All'attach HID: nessuna notifica VFS — ring SPSC già attivo

---

## Dipendenze Backlog 2 (aggiornate)

```
M7-01 (IPC RT) ──────────────────────────────────────────────┐
M7-02 (shell nsh)                                            │
M6-01/02 (ELF loader + execve) ─────────────────────────────┤
                                                             │
M8-01 (fork/COW) ←──────────────────────────────────────────┘
M8-01 → M8-02 (mmap file)
M8-01 → M8-03 (signal)
M8-03 → M8-04 (job control)
M3-01 + M2-03 + M1-02 → M8-05 (mreact)
M3-01 + M2-03 + M2-02 → M8-06 (ksem — semafori kernel)
M8-06 + M2-03 → M8-07 (kmon — monitor kernel)
M8-01 + M6-02 + M11-01 + M8-03 + M5-02 + M4-03 → M8-08 (arksh porting — shell default)
M9-01 (capability) → M9-02 (vfsd) → M9-03 (blkd) → M9-04 (namespace)
M10-01 (virtio-net) → M10-02 (TCP/IP) → M10-03 (socket API)
M6-01 + M8-01 + M8-08a + M8-08b + M8-08c + M9-01 → M11-01 (musl libc)
M11-01 + M8-01 + M8-06 + M8-07 → M11-02 (pthread + futex + sem_t POSIX + monitor C++)
M11-02 → M11-03 (dynamic linker)
M6-01 + M11-01 + M11-02 + M11-03 → M11-04 (Mach-O compat)
M6-01 + M11-01 + M11-03 + M8-08a + M8-08b + M8-06 + M14-01 → M11-05 (Linux compat)
M11-05 + M10-03 + M5-03 → M11-06 (io_uring — bassa priorità)
M11-05 + M9-04 + M10-01 → M11-07 (namespace + container — bassa priorità)
M11-07 → M11-07e (OverlayFS) → M11-07f (cgroup v2)
M11-01 + M9-02 → M12-01 (wld wayland server)
M12-01 → M12-02 (wm) → M12-03 (GPU shader compositor)
M2-03 → M13-01 (EDF scheduler)
M2-03 → M13-02 (SMP)
M13-02 → M13-03 (WCET framework)
M2-03 + M3-03 (ANE) → M13-04 (NAS profiler + FPP advisor)
M13-01 + M13-04 → M13-05 (NAS-EDF integration)
M9-02 → M14-01 (procfs/sysfs)
M6-01 → M14-02 (crash reporter)
M13-02 → M14-03 (power management)
/* Audio */
M2-01 (GIC) → M15-01 (virtio-sound driver)
M2-01       → M15-02 (PL041 fallback)
M15-01 + M9-01 + M8-03 → M15-03 (audiod server)
M15-03 → M15-04 (audio syscall 140-148)
M15-03 → M15-05 (codec DSP + resampling)
/* USB */
M9-01 (CAP_MMIO) → M16-01 (XHCI driver)
M16-01 → M16-02 (hub manager)
M16-01 → M16-03 (HID — tastiera/mouse USB)
M16-01 → M16-04 (MSC — mass storage)
M16-04 + M9-02 → M16-05 (FAT32)
M16-01 → M16-06 (CDC-ACM — seriale USB)
M16-01 → M16-07 (UVC — webcam)
M16-01 + M16-02 + M16-03 + M16-04 + M16-06 → M16-08 (usbd daemon)
```

---

## Prossimi tre step consigliati

1. **M10-01** VirtIO Network Driver — apre l'intera traccia networking e sblocca socket/API BSD
2. **M8-08 plugin** — ora che `M11-03` e' chiusa, i plugin dinamici di `arksh` sono il prossimo passo shell-side sensato
3. **M8-08h** i18n / localizzazione stringhe — completa il salto UX dopo i layout tastiera `M8-08g`

Dopo M8-01 + M11-01 è possibile compilare e avviare programmi C esistenti non modificati.
Dopo M11-04 binari Mach-O AArch64 compilati per macOS girano su EnlilOS senza recompilazione.
Dopo M11-05 binari Linux AArch64 (glibc e musl) girano senza recompilazione — bash, python3, git, gcc.
Dopo M11-06 (io_uring) server I/O-intensi ottengono 2-4× throughput rispetto a epoll.
Dopo M11-07 (namespace + cgroup) Docker/Podman AArch64 gira nativamente su EnlilOS.
Dopo M12-01 + M12-02 EnlilOS ha un desktop funzionante con finestre Wayland.
Dopo M13-02 (SMP) il kernel usa tutti e 4 i core QEMU: throughput 4×.
Dopo M15-01..04 il sistema ha output audio funzionante con mixing multi-client.
Dopo M16-01..08 chiavette USB, tastiere/mouse USB e dispositivi seriali USB funzionano via `usbd`.

---

---

# Piano di Sviluppo — Sequenza Raccomandata

> **Criterio di ordinamento:** sbloccare il massimo numero di milestone successive
> con il minimo lavoro, procedendo sempre dal kernel verso l'userspace.
> Le fasi sono sequenziali; all'interno di ogni fase le milestone sono parallelizzabili.

---

## FASE 1 — Fondamenta Kernel (prerequisiti di tutto)
**Obiettivo:** il kernel può lanciare processi, gestire la memoria per processi multipli
e ha strumenti di debug decenti. Senza questa fase nessuna altra funziona.

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 1 | **M14-02** Crash Reporter + KGDB | Prima di tutto: rende il resto debuggabile. Stack trace + watchdog ti salvano ore |
| 2 | **M8-01** fork() + COW MMU | Sblocca ogni altra milestone — senza fork nessun processo utente può creare figli |
| 3 | **M8-03** Signal Handling | Richiesto da shell, job control, librerie POSIX. Dipendenza transitiva di metà del backlog |
| 4 | **M8-05** mreact | Solo kernel, zero dipendenze esterne. Primitiva unica — meglio averla subito prima che tutto si costruisca sopra pattern di polling |
| 5 | **M8-06** ksem | Semplice, solo kernel. Sblocca M8-07 e diventa base di POSIX sem_t |
| 6 | **M8-07** kmon | Monitor kernel — dipende solo da M8-06. Chiude il quadro delle primitive di sincronizzazione |

**Checkpoint FASE 1:** il sistema fa boot, lancia due processi figli via fork, i segnali
funzionano, le primitive di sync sono disponibili, il panic stampa uno stack trace leggibile.

---

## FASE 2 — Capability System e Server Architecture
**Obiettivo:** spostare il codice di sistema in server user-space isolati.
Senza questa fase tutto il kernel è un blob monolitico non sicuro.

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 7 | **M9-01** Capability System | Gia' implementata v1 nel kernel; base di sicurezza pronta prima di muovere driver e VFS fuori dal kernel |
| 8 | **M9-02** VFS Server (`vfsd`) | Migra il VFS kernel di M5-02 in user-space. Dipendenza critica di audio, USB, procfs |
| 9 | **M9-03** Block Server (`blkd`) | Migra il driver virtio-blk. Dipende da M9-02 e M9-01 |
| 10 | **M14-01** procfs / sysfs | `procfs` core gia' implementato; resta da estendere `sysfs` e gli export avanzati |
| 11 | **M9-04** Mount Dinamico + Namespace | ✅ Completata v1. `vfsd` gestisce cwd, mount privati, bind mount e `pivot_root()` |

**Checkpoint FASE 2:** VFS e block device girano come processi separati, il filesystem
è montato via server IPC, `/proc` è navigabile da shell.

---

## FASE 3 — Libc e Runtime C
**Obiettivo:** avere una toolchain funzionante che produce binari che girano su EnlilOS.
Da qui in poi si può iniziare a portare software esistente.

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 12 | **M8-08a** pipe() + dup/dup2 | ✅ Completata v1. Refcount corretto su dup/fork, `POSIXDEMO.ELF`, selftest `posix-ux` |
| 13 | **M8-08b** getcwd/chdir/env | ✅ Completata v1. `cwd` namespace-aware via `vfsd`, env bootstrap per spawn |
| 14 | **M8-08c** termios + isatty | ✅ Completata v1. Canonical/raw sulla console globale, subset termios sufficiente al bootstrap |
| 15 | **M11-01** musl libc | ✅ Completata v1: ABI minima, TLS/startup, sysroot/toolchain bootstrap. Suite attuale `43/43` |
| 16 | **M8-08e** Build System e Toolchain | ✅ Completata v1: toolchain CMake, target `arksh-*`, compat shim di riferimento e smoke `/ARKSHSMK.ELF` |
| 17 | **M8-08f + M8-08** integrazione shell di default | ✅ Completata v1: `/bin/arksh` launcher, auto-login shell, bind `/data/home -> /home`, rc bootstrap, fallback `/bin/nsh`, selftest `arksh-login` |

Nota: **M8-04 Job Control** e' gia' completata e disponibile come base per `CTRL+Z`,
foreground/background group e `waitpid(WUNTRACED)`.

**Checkpoint FASE 3:** si può compilare un programma C con musl, eseguirlo su EnlilOS,
entrare nella login shell `arksh` `v1` (con fallback `nsh`), usare pipe
(`ls | grep .c`) e validare il bootstrap shell-side end-to-end.

---

## FASE 4 — Threading e Sincronizzazione POSIX
**Obiettivo:** supporto thread completo. Richiesto da praticamente ogni programma C++,
Python, server applicativi.

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 19 | **M11-02** pthread + futex + sem_t | ✅ Completata v1: `M11-02a+b+c+d+e` chiuse (`tgid/gettid`, `clone()` subset, `proc_slot` condiviso, `set_tid_address/exit_group/tgkill`, `futex WAIT/WAKE/REQUEUE`, wrapper musl `pthread`/`sem_t`, TLS statico multi-thread da `PT_TLS`, `errno` thread-local, `clone-thread`, `thread-lifecycle`, `futex-core`, `musl-pthread`, `musl-sem`, `tls-mt`) |
| 20 | **M8-02** mmap file-backed | Gia' implementata v1. Resta utile come base per `pthread`, libc e carichi user-space piu' grandi |
| 21 | **M11-03** Dynamic Linker | ✅ Completata v1: `libdl`, `dlopen/dlsym/dlclose/dlerror`, smoke `musl-dlfcn`, base runtime `.so` |
| 22 | **M8-08 plugin** arksh plugin system | Adesso che il dynamic linker c'è, i plugin `.so` di arksh funzionano |

**Checkpoint FASE 4:** programmi multi-thread compilano e girano, le `.so` si caricano
a runtime, arksh carica plugin dal filesystem.

---

## FASE 5 — Rete
**Obiettivo:** connettività di rete. Richiesta da package manager, `curl`, `git`, SSH.

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 23 | **M10-01** VirtIO Network Driver | Driver hardware. Base di tutto il networking |
| 24 | **M10-02** TCP/IP (lwIP) | Stack di rete. Dipende solo dal driver |
| 25 | **M10-03** BSD Socket API | Espone la rete a user-space. Sblocca `curl`, `wget`, SSH |

**Checkpoint FASE 5:** `ping 1.1.1.1` funziona, `curl` scarica una pagina,
un server TCP di test accetta connessioni.

---

## FASE 6 — Compatibilità Binaria
**Obiettivo:** eseguire binari esistenti senza ricompilazione. Massimizza il software
disponibile su EnlilOS dal giorno zero.

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 26 | **M11-05** Linux AArch64 Compat | Dipende da M11-03 + M10-03 + M14-01. Il guadagno è immediato: bash, python3, git, gcc |
| 27 | **M11-04** Mach-O Compat | Dipende da M11-03. Binari macOS AArch64 senza recompilazione |

**Checkpoint FASE 6:** `bash` scaricato da un sistema Linux AArch64 gira su EnlilOS
senza modifiche. `python3 -c "print('hello')"` funziona.

---

## FASE 7 — Display e Desktop
**Obiettivo:** interfaccia grafica. Dipende dalla GPU già implementata (M5b).

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 28 | **M12-01** Wayland Server (`wld`) | Dipende da M11-01 + M9-02 + M10-03 (AF_UNIX). Il compositor |
| 29 | **M12-02** Window Manager | Dipende da M12-01. Finestre, decorazioni, tiling |
| 30 | **M12-03** GPU Shader Compositor | Dipende da M12-01 + M5b-04. Compositing accelerato GPU |

**Checkpoint FASE 7:** arksh gira in una finestra Wayland, si possono aprire più
terminali affiancati, il cursore del mouse sposta il focus.

---

## FASE 8 — Scheduler Avanzato e Multi-Core
**Obiettivo:** sfruttare tutti i core disponibili, garanzie RT formali e adattività via NPU.

| Ordine | Milestone | Perché adesso |
|--------|-----------|---------------|
| 31 | **M13-02** SMP multi-core | Dipende solo da M2-03 (già fatto). Tutti e 4 i core QEMU attivi |
| 32 | **M13-03** WCET Framework | Dipende da M13-02. Misura le performance in modo rigoroso |
| 33a | **M13-01** EDF Scheduler | Dipende da M2-03. Garanzie RT formali, 100% utilization teorica |
| 33b | **M13-04** NAS Profiler + FPP Advisor | Parallelo a M13-01. Profiler lockless + advisor NPU su priorità FPP |
| 34 | **M13-05** NAS-EDF Integration | Dipende da M13-01 + M13-04. WCET adattivo, CBS dinamico, EDF+NAS |

**Checkpoint FASE 8:** SMP 4 core attivi, statistiche WCET in `/proc/wcet`,
EDF operativo con test schedulabilità RMS, NAS riduce miss rate soft-RT del 15–30%.

**Nota architetturale:** EDF e NAS sono complementari, non alternativi.
EDF risponde a "chi esegue adesso?" (selezione deterministica).
NAS risponde a "quanto peso dare a ciascuno?" (stima parametri adattiva).
Vedere `docs/STUDIO_SCHEDULER_EDF_NAS.md` per l'analisi completa.

---

## FASE 9 — Audio e USB
**Obiettivo:** periferiche fisiche complete. Possono procedere in parallelo tra loro.

| Ordine | Milestone | Note |
|--------|-----------|------|
| 34a | **M15-01** VirtIO Sound Driver | **parallelo con M16-01** |
| 34b | **M16-01** XHCI USB Driver | **parallelo con M15-01** |
| 35a | **M15-03** audiod server | Dipende da M15-01 |
| 35b | **M16-02** Hub Manager | Dipende da M16-01 |
| 36a | **M15-04** Audio Syscall | Dipende da M15-03 |
| 36b | **M16-03** HID (tastiera/mouse USB) | Dipende da M16-02 |
| 37 | **M16-04** MSC (chiavette USB) | Dipende da M16-02 |
| 38 | **M16-05** FAT32 | Dipende da M16-04 + M9-02 |
| 39 | **M16-08** usbd daemon | Integra tutti i driver USB |
| 40 | **M15-05** Codec DSP | Dipende da M15-03. Resampling, EQ, OPUS |
| 41 | **M16-06** CDC-ACM (USB seriale) | Dipende da M16-01 |

**Checkpoint FASE 9:** musica in riproduzione via `aplay`, chiavetta USB montata
automaticamente in `/usb0`, tastiera USB funzionante come tastiera PS/2.

---

## FASE 10 — Bassa Priorità (opzionale, in ordine di utilità)

| Milestone | Sblocca |
|-----------|---------|
| **M14-03** Power Management | `poweroff` e `reboot` da shell |
| **M14-04** VMware Fusion Image Builder | immagini `.vmdk`/`.ova` bootabili per test e distribuzione |
| **M16-07** UVC Webcam | Streaming video da webcam USB |
| **M15-02** PL041 fallback | Audio su QEMU senza virtio-sound |
| **M11-06** io_uring | Throughput server I/O-intensi 2-4× |
| **M11-07a..d** Namespace (Mount/PID/UTS/Net) | Isolamento processi |
| **M11-07e** OverlayFS | Image layering Docker |
| **M11-07f** Cgroup v2 | Limite risorse per container |
| **M13-01** EDF + **M13-04/05** NAS | Già in Fase 8 — EDF+NAS ibrido, opzionale se FPP è sufficiente |

**Checkpoint FASE 10:** Docker AArch64 gira su EnlilOS, container isolati con
limite memoria e CPU, `docker pull` scarica un'immagine e la esegue.

---

## Vista complessiva per milestone critica

```
FASE 1  ──► fork + signal + ksem/kmon + crash reporter
   │
FASE 2  ──► capability + vfsd + blkd + procfs
   │
FASE 3  ──► musl libc + pipe/dup + termios + arksh
   │
FASE 4  ──► pthread + futex + mmap file + dynamic linker
   │
FASE 5  ──► virtio-net + TCP/IP + socket
   │
FASE 6  ──► Linux compat + Mach-O compat
   │
FASE 7  ──► Wayland + WM + GPU compositor
   │
FASE 8  ──► SMP + WCET + EDF + NAS (EDF+NAS scheduler ibrido)
   │
FASE 9  ──► audio + USB (in parallelo)
   │
FASE 10 ──► container + io_uring + power (opzionale)
```

**Milestone bloccanti** (fermare tutto e risolvere prima di procedere):
- ~~M8-01 fork — senza fork nulla funziona~~ ✅ completata
- ~~M9-01 capability — senza capability i server non sono sicuri~~ ✅ completata
- ~~M11-01 musl libc — senza libc nessun programma C gira~~ ✅ completata v1
- ~~M14-02 crash reporter — senza debug ogni bug diventa un'ora di lavoro in più~~ ✅ completata

**Stato FASE 1, FASE 2 e avvio FASE 3 al 2026-04-10:**
- ✅ M8-01 fork + COW
- ✅ M8-03 signal handling
- ✅ M8-05 mreact
- ✅ M8-06 ksem
- ✅ M8-07 kmon (race condition selftest corretta)
- ✅ M9-01 capability system
- ✅ M9-02 vfsd bootstrap v1
- ✅ M8-04 process groups / sessions / job control
- ✅ M8-08a pipe + dup/dup2
- ✅ M8-08b getcwd/chdir + env bootstrap
- ✅ M8-08c termios + isatty
- ✅ M9-03 blkd
- ✅ M9-04 namespace + mount dinamico v1
- ✅ M11-01 musl/toolchain bootstrap v1
- ✅ M8-08e build/toolchain arksh v1
- ✅ M8-08f integrazione shell/login v1
- **Prossimo step:** aprire la rete base con `M10-01`, poi plugin arksh dinamici, poi completare l'i18n con `M8-08h`

---

## Prossimi passi — Progress Log Operativo Aggiornato

> Questa sezione sostituisce operativamente gli snapshot piu' vecchi sopra.
> Stato verificato dopo la chiusura di `M8-08g`: suite `selftest` a `44/44`.

### 1. Cosa e' gia' stato completato

- ✅ **Fondamenta kernel e debug**: `M14-02`, `M8-01`, `M8-03`, `M8-04`, `M8-05`, `M8-06`, `M8-07`
- ✅ **Architettura server / storage v1**: `M9-01`, `M9-02`, `M9-03`, `M9-04`, `M14-01` (`procfs` core v1)
- ✅ **Runtime C / POSIX bootstrap v1**: `M8-02`, `M8-08a`, `M8-08b`, `M8-08c`, `M8-08d`, `M8-08e`, `M8-08f`, `M8-08g`, `M11-01a`, `M11-01b`, `M11-01c`, `M11-03`
- ✅ **Threading POSIX bootstrap v1**: `M11-02a`, `M11-02b`, `M11-02c`, `M11-02d`, `M11-02e`
- ✅ **Stato validato**: processi, namespace VFS, `musl` bootstrap, `pthread`, `sem_t`, `futex`, TLS multi-thread, `errno` thread-local

### 2. Cosa resta da fare ad alta priorita'

| Priorita' | Milestone | Dipende da | Perche' viene adesso |
|-----------|-----------|------------|----------------------|
| 1 | **M10-01** VirtIO Network Driver | nessuna dipendenza forte oltre al core gia' chiuso | Apre l'intera traccia networking e toglie il sistema dal solo bootstrap locale |
| 2 | **M8-08 plugin** | `M11-03` | Ora che `libdl` c'e', i plugin dinamici della shell diventano finalmente sensati |
| 3 | **M10-02** TCP/IP Stack | `M10-01` | Senza stack IP non esiste networking utile in user-space |
| 4 | **M10-03** BSD Socket API | `M10-02` | Sblocca `curl`, `ssh`, package manager, servizi e AF_UNIX/AF_INET consistenti |
| 5 | **M8-08h** i18n stringhe | `M8-08g` | Evita che la UX shell/desktop resti solo `en_US`/hardcoded dopo aver chiuso i layout |
| 6 | **M11-05** Linux compatibility layer | `M11-03 + M10-03 + M14-01` | Diventa molto piu' interessante appena la rete base e' disponibile |

### 3. Sequenza raccomandata per dipendenze

1. **Portare la shell oltre il bootstrap**: `M8-08 plugin`
2. **Aprire la rete**: `M10-01 -> M10-02 -> M10-03`
3. **Migliorare l'usabilita' shell/input**: `M8-08h`
4. **Usare la rete per compatibilita' e desktop**:
   `M11-05` dipende da `M11-03 + M10-03 + M14-01`
   `M12-01` dipende da `M11-01 + M9-02 + M10-03`
5. **Scalare su multicore e RT avanzato**:
   `M13-02 -> M13-03`
   `M13-01` e `M13-04` possono procedere in parallelo dopo il core SMP
   `M13-05` chiude l'integrazione EDF+NAS

### 4. Tracce che si aprono subito dopo i prossimi blocchi

- **Compatibilita' Linux reale**: `M11-05`
  Dipende da `M11-03 + M10-03 + M14-01`
  Effetto: porta `bash`, `python3`, `git`, `gcc` in modo molto piu' credibile

- **Desktop grafico**: `M12-01 -> M12-02 -> M12-03`
  Dipende in pratica da rete/socket, `vfsd` e GPU gia' disponibile
  Effetto: porta Wayland, WM e compositor GPU

- **SMP e scheduler avanzati**: `M13-02 -> M13-03 -> (M13-01 || M13-04) -> M13-05`
  Effetto: throughput multicore, metriche WCET e scheduling RT piu' forte

- **Periferiche complete**: `M15-*` e `M16-*`
  Possono partire dopo rete/base userspace consolidata, con audio e USB in parallelo

### 5. Punti aperti ma non bloccanti subito

- `M14-01` e' **completata v1**, ma resta da estendere `sysfs` e alcuni export avanzati
- `M11-01`, `M11-02` e `M11-03` sono **chiuse v1**; il salto qualitativo successivo ormai sta in plugin shell, networking e compat piu' ampia
- La priorita' pratica non e' aggiungere altre primitive kernel isolate, ma **chiudere shell reale + linking dinamico + networking**

### 6. Ordine operativo consigliato da qui

1. `M8-08 plugin`
2. `M10-01`
3. `M10-02`
4. `M10-03`
5. `M8-08h`
6. `M11-05`
7. `M12-01`
8. `M13-02`

Se serve un principio guida unico: **prima rendere EnlilOS un sistema usabile da shell reale,
poi un sistema con librerie dinamiche, poi un sistema con rete, e solo dopo un sistema desktop
e multi-core pienamente general-purpose.**
