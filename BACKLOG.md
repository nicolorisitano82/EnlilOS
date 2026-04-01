# EnlilOS — Backlog Implementazioni
## Sistema Real-Time Microkernel su AArch64

---

## Principi RT che guidano ogni decisione

Ogni milestone deve rispettare questi vincoli. Sono non negoziabili.

| Principio | Regola pratica |
|---|---|
| **Latenza deterministica** | Nessun ciclo con WCET illimitato nei percorsi critici |
| **No demand paging** | Tutta la memoria del kernel è pre-allocata e pre-mappata |
| **No allocazione dinamica nei path IRQ** | `kmalloc` vietato dentro handler di interrupt |
| **Priorità preemptiva a cascata** | Un task ad alta priorità interrompe sempre uno a bassa, anche dentro il kernel |
| **Priority Inheritance** | Nessuna inversione di priorità su mutex e IPC |
| **Memoria locked** | Nessun page fault nel kernel — zero TLB miss inatteso |
| **WCET misurabile** | Ogni funzione critica ha un upper bound noto in cicli |
| **Static = preferibile** | Pool statici > allocazione dinamica per tutto ciò che è in hot path |

---

## MILESTONE 1 — Kernel Foundation

### ✅ M1-01 · Exception Vector Table
**Stato:** COMPLETATA
16 handler (4 sorgenti × 4 tipi), salvataggio frame completo (288 byte), dump diagnostico su UART con decodifica ESR_EL1. Entry da 5 istruzioni + branch out-of-line.
**Nota RT:** latenza ingresso vector table = WCET fisso (5 istr. + branch predictor warm).

---

### ✅ M1-02 · MMU e Virtual Memory
**Stato:** COMPLETATA
Page table L1-only (T0SZ=25, block da 1GB). MMIO → Device-nGnRnE, RAM → Normal WB. D-cache + I-cache abilitate. `mmu_prefault_range()` per warm-up TLB pre-avvio task RT.
**Nota RT:** TLB miss WCET = 1 memory access (nessun walk L2/L3). Cache abilitate → accessi kernel ~3-10 cicli invece di ~100.

---

### ✅ M1-03 · Physical Memory Allocator
**Priorità:** CRITICA

**Stato:** COMPLETATA
Buddy 11 ordini (4KB–4MB) con free list circolari doppie. Slab 7 classi (32–2048B) con page header per kfree O(1). 566 pagine riservate al kernel, 509MB liberi. `slab_warm()` pre-popola 315+93+133 oggetti al boot.
**RT:** `phys_alloc_page()` ≤ 11 iter. `kmalloc()` = pop da free list = O(1). `kfree()` = push = O(1).

**RT design:** due allocatori distinti con WCET differenti.

**Allocatore a buddy (bulk):** per allocazioni grandi (>=4KB), O(log N).
**Allocatore a slab (hot path):** pool a dimensione fissa per strutture dati frequenti
(`task_t`, `ipc_message_t`, `port_t`). Allocazione/rilascio in O(1), no coalescing.

- Bitmap sulla RAM fisica (QEMU 512MB = 131072 pagine da 4KB)
- `phys_alloc_page()` / `phys_free_page()` — O(1) grazie a free-list con testa
- `phys_alloc_pages(n)` — O(log N) buddy per blocchi contigui
- Sezioni riservate marcate al boot: kernel image, framebuffer, page tables, stack

**Vincolo RT:** `phys_alloc_page()` deve completare entro 100 cicli nel caso peggiore.

---

### ✅ M1-04 · Kernel Heap — named typed caches
**Stato:** COMPLETATA

**RT design:** livello named cache sopra il slab allocator di M1-03. Ogni struttura kernel
fondamentale ha una cache dedicata con nome, free list propria e limite configurabile.

- `kmem_cache_t`: nome, obj_size, max_objects, free list intrusive (8B overhead = 0 per oggetto)
- `task_cache` 64B×64, `port_cache` 64B×128, `ipc_cache` 512B×128 — pre-caldi al boot
- `kmem_cache_alloc()` O(1) hot path, O(≤MAX_ORDER) al cold refill (solo fuori da RT)
- `kmem_cache_free()` O(1) sempre — push in testa alla free list
- `kfree()` esteso: magic check (double-free/corruption → panic), path SLAB_CLASS_LARGE → phys_free_pages
- Overflow limite → kernel panic con nome cache, mai silenzioso

---

## MILESTONE 2 — Interrupt e Timer RT

### ✅ M2-01 · GIC-400 — Interrupt Controller
**Stato:** COMPLETATA

**RT design:** tabella di dispatch `irq_table[256]` indicizzata direttamente sull'IRQ ID.
Lookup O(1), nessuna lista, nessuna ricerca, nessuna allocazione nel hot path.

- GICD @ `0x08000000`, GICC @ `0x08010000`
- 256 linee IRQ rilevate da `GICD_TYPER.ITLinesNumber` al boot
- Priorità hardware 0–255: `GIC_PRIO_REALTIME=0x40`, `GIC_PRIO_DRIVER=0x80`, `GIC_PRIO_MIN=0xF0`
- `gic_handle_irq()`: IAR read → bounds check → call → EOIR write = WCET costante (3 MMIO + 1 call)
- `gic_register_irq()`: O(1) — scrittura array + 3 MMIO, chiamare al boot
- `gic_enable_irqs()` / `gic_disable_irqs()`: inline `msr daifclr/daifset, #2`
- IRQ UART0 (#33) registrato e abilitato; zero spurious interrupt al boot
- `irq_handler()` in `exception.c` ora chiama `gic_handle_irq()` (non più stub)

---

### ✅ M2-02 · ARM Generic Timer
**Stato:** COMPLETATA

**RT design:** Physical EL1 Timer (CNTP) con reload via CNTP_TVAL — nessun drift accumulato.

- `CNTFRQ_EL0 = 62500000 Hz (62.5 MHz)` — letto dal firmware QEMU al boot
- Tick: **1ms (1000 Hz)** — period = 62500 cicli, reload a ogni IRQ
- `timer_now_ns()` O(1): `(cntpct * ns_per_tick_frac) >> 32`, nessuna divisione
- `timer_now_ms()` O(1): legge `jiffies` con barriera di memoria
- `timer_delay_us()`: busy-wait via CNTPCT_EL0, solo per uso init
- IRQ PPI #30 registrato nel GIC prio=`REALTIME=0x40`, edge-triggered
- `timer_set_tick_callback(fn)`: hook per lo scheduler M2-03
- Verificato su QEMU: 8 tick in ~10ms, elapsed ~10.041ms, errore < 0.5%

---

### ✅ M2-03 · Scheduler Real-Time — Fixed-Priority Preemptive
**Stato:** COMPLETATA

**RT design:** FPP con 256 priorità, ready bitmap 256 bit, O(1) ricerca + switch.

- `sched_tcb_t` (64B — fit in task_cache): sp, pid, priority, state, ticks_left, runtime_ns, deadline_ms
- `ready_bitmap[4]` (256 bit): `__builtin_ctzll` → trova la priorità massima in O(1)
- `run_queue[256]`: FIFO singly-linked per priorità, O(1) enqueue/dequeue
- `sched_context_switch()` in assembly: salva x19-x28/x29/x30 (96B), swap SP → `ret`
- `task_entry_trampoline`: abilita IRQ + salta a entry function (prima schedulazione)
- **Preemption hardware**: `vectors.S` legge `need_resched` dopo ogni `exception_handler()` → chiama `schedule()` → ERET al nuovo task
- `sched_tick()` O(1): decrementa quantum, setta `need_resched` a ogni 1ms
- Task demo: `kernel` (prio=0), `idle` (prio=255, WFE loop), `ticker` (prio=32, 500ms)
- Verificato: ticker stampa ogni 500ms, stats ogni 2s, runtime contabilizzato correttamente

---

## MILESTONE 3 — Syscall Interface

### ✅ M3-01 · Syscall Dispatcher
**Stato:** COMPLETATA

**RT design:** tabella `syscall_table[256]` indicizzata direttamente: O(1), no ricerca.

- Handler per `EC = 0x15` (SVC AArch64) in `exception_handler()` → `syscall_dispatch()`
- Numero syscall in `x8`, argomenti `x0–x5`, ritorno `x0` (ABI Linux AArch64)
- Flag `SYSCALL_FLAG_RT` / `SYSCALL_FLAG_NOBLOCK` per classificazione RT
- Stub RT-safe implementati: `write` (fd=1/2→UART), `read` (fd=0→EAGAIN), `exit` (zombie+block), `clock_gettime` (CNTPCT_EL0 O(1))
- ENOSYS automatico per qualsiasi nr >= 256 o non registrato
- `syscall_init()` chiamato dopo `sched_init()` al boot

---

### ✅ M3-02 · Syscall Base (13 syscall)
**Stato:** COMPLETATA

| Nr | Nome | RT-safe | Implementazione |
|----|------|---------|-----------------|
| 1 | `write` | Sì | fd=1/2 → UART; fd_table per tipo; `/dev/null` → discard |
| 2 | `read` | Sì (non-blocking) | fd=0 → EAGAIN (keyboard M4-01); `/dev/null` → EOF |
| 3 | `exit` | Sì | TCB_STATE_ZOMBIE + sched_block(); non ritorna |
| 4 | `open` | No | Risoluzione path via VFS (`/`, `/dev`, `/data`, `/sysroot`) |
| 5 | `close` | Sì | Libera slot fd; protegge fd 0/1/2 |
| 6 | `fstat` | No | `stat_t` minimale popolata dal backend VFS |
| 7 | `mmap` | No | MAP_ANONYMOUS: phys_alloc_pages(order) + zero-fill; PA==VA (identity map) |
| 8 | `munmap` | No | phys_free_pages(addr, order) |
| 9 | `brk` | Sì | Per-task break pointer, area HEAP_BASE + idx×4MB; lazy-init; O(1) |
| 10 | `execve` | — | ENOSYS (richiede ELF loader M6-02) |
| 11 | `fork` | — | ENOSYS (richiede COW MMU M6+) |
| 12 | `waitpid` | Sì (timed) | WNOHANG=polling; timeout_ms>0=bounded wait; sched_yield nel loop |
| 13 | `clock_gettime` | Sì — O(1) | Scrive `{tv_sec, tv_nsec}` nel puntatore timespec; CNTPCT_EL0 |

**Strutture aggiuntive:**
- `fd_table[32][16]`: file descriptor per task, indicizzato su `pid % 32`; fd 0/1/2 preinizializzati CONSOLE
- `task_brk[32]`: program break per task; HEAP_BASE=0x60000000, 4MB/task
- `sched_task_find(pid)`: ricerca O(N) nel task_pool — usata da waitpid
- `timespec_t { int64_t tv_sec; int64_t tv_nsec }`: scrivibile da clock_gettime

---

### ✅ M3-04 · GPU Syscall Interface — Apple AGX / Virtio-GPU
**Priorità:** ALTA

**Obiettivo:** esporre il chip grafico Apple AGX (M-series) o virtio-GPU (QEMU) in modo
diretto a user-space via syscall, bypassando il server grafico per task che necessitano
accesso GPU a bassa latenza (real-time rendering, compute shader, scanout diretto).

**Contesto hardware Apple AGX:**
L'Apple AGX è il nome della GPU integrata in tutti i chip M-series. Non ha una specifica
pubblica — il driver open-source di riferimento è `asahi-gpu` (Asahi Linux). La GPU espone:
- Command queues (CQ) tramite MMIO e shared memory ring buffer
- Memoria GPU gestita tramite IOMMU (DART su M-series)
- IRQ completamento via AIC (Apple Interrupt Controller)
- Tile-based deferred rendering (TBDR) con vertex/fragment shader ISA proprietaria

**RT design:**
- Submit command buffer **non-blocking**: deposita nel ring e ritorna subito — O(1)
- Wait con **deadline**: `gpu_wait()` bounded — mai unbounded in task RT
- Buffer GPU pre-allocati al boot: zero allocazione nel hot path
- Scanout diretto: `gpu_present()` fa page-flip atomico senza passare dal compositor
- Software fallback su QEMU (`gpu_sw.c`): rasterizzazione CPU per sviluppo

**Numeri syscall (range dedicato 120–139 — separati da ANE 100–119):**

| Nr  | Nome                   | RT-safe | Firma C                                                             | Note |
|-----|------------------------|---------|---------------------------------------------------------------------|------|
| 120 | `gpu_query_caps`       | Sì — O(1)   | `(gpu_caps_t *out)`                                             | Versione chip, VRAM, feature flags |
| 121 | `gpu_buf_alloc`        | No          | `(size_t size, uint32_t type) → gpu_buf_handle_t`               | Alloca GPU buffer object (GBO); solo al setup |
| 122 | `gpu_buf_free`         | Sì — O(1)   | `(gpu_buf_handle_t h)`                                          | Rilascio a pool pre-allocato |
| 123 | `gpu_buf_map_cpu`      | No          | `(gpu_buf_handle_t h) → void *`                                 | Mappa GBO in CPU address space (identity map) |
| 124 | `gpu_buf_unmap_cpu`    | Sì — O(1)   | `(gpu_buf_handle_t h)`                                          | Invalida cache CPU, rilascia mapping |
| 125 | `gpu_cmdqueue_create`  | No          | `(uint32_t type, uint32_t depth) → gpu_queue_handle_t`          | Crea command queue (RENDER / COMPUTE / BLIT) |
| 126 | `gpu_cmdqueue_destroy` | No          | `(gpu_queue_handle_t q)`                                        | |
| 127 | `gpu_cmdbuf_begin`     | **Sì**      | `(gpu_queue_handle_t q) → gpu_cmdbuf_t *`                       | Inizia registrazione comandi; ritorna puntatore al command buffer |
| 128 | `gpu_cmdbuf_submit`    | **Sì**      | `(gpu_cmdbuf_t *cb, uint32_t prio) → gpu_fence_t`               | Submit non-blocking; ritorna fence per sincronizzazione |
| 129 | `gpu_fence_wait`       | **Sì**      | `(gpu_fence_t f, uint64_t timeout_ns) → int`                    | Attende completamento con timeout; 0 = polling |
| 130 | `gpu_fence_query`      | Sì — O(1)   | `(gpu_fence_t f) → int`                                         | Controlla stato fence senza bloccare |
| 131 | `gpu_fence_destroy`    | Sì — O(1)   | `(gpu_fence_t f)`                                               | |
| 132 | `gpu_present`          | **Sì**      | `(gpu_buf_handle_t scanout, uint32_t x, uint32_t y, uint32_t w, uint32_t h) → gpu_fence_t` | Page-flip atomico: submette scanout buffer al display engine |
| 133 | `gpu_compute_dispatch` | **Sì**      | `(gpu_queue_handle_t q, gpu_buf_handle_t shader, uint32_t gx, uint32_t gy, uint32_t gz, gpu_buf_handle_t args) → gpu_fence_t` | Dispatch compute shader; non-blocking |

**Strutture dati principali:**

```c
/* Capacità GPU */
typedef struct {
    uint32_t  vendor;           /* GPU_VENDOR_APPLE_AGX / GPU_VENDOR_VIRTIO */
    uint32_t  device_id;        /* es. 0x6000 = M1, 0x6020 = M2, ... */
    uint64_t  vram_bytes;       /* VRAM disponibile (shared con sistema su M-series) */
    uint32_t  max_texture_dim;  /* dimensione massima texture (es. 16384) */
    uint32_t  compute_units;    /* numero compute unit (shader core) */
    uint32_t  flags;            /* GPU_CAP_COMPUTE | GPU_CAP_RT | GPU_CAP_SWFALLBACK */
} gpu_caps_t;

/* Handle opachi (indici in pool statico) */
typedef uint32_t gpu_buf_handle_t;
typedef uint32_t gpu_queue_handle_t;
typedef uint64_t gpu_fence_t;          /* fence ID, 0 = invalid */

/* Tipi di GPU buffer */
#define GPU_BUF_VERTEX      (1 << 0)
#define GPU_BUF_TEXTURE     (1 << 1)
#define GPU_BUF_UNIFORM     (1 << 2)
#define GPU_BUF_STORAGE     (1 << 3)
#define GPU_BUF_SCANOUT     (1 << 4)   /* display scanout buffer */
#define GPU_BUF_SHADER      (1 << 5)   /* compiled shader binary */

/* Tipi di command queue */
#define GPU_QUEUE_RENDER    0
#define GPU_QUEUE_COMPUTE   1
#define GPU_QUEUE_BLIT      2          /* copia/fill accelerata */

/* Command buffer (puntatore diretto al ring buffer GPU-mapped) */
typedef struct {
    uint32_t  *cmds;        /* puntatore ai comandi GPU (write-combined) */
    uint32_t   capacity;    /* numero massimo di comandi */
    uint32_t   count;       /* comandi registrati finora */
    uint32_t   queue_idx;   /* index nella gpu_queue */
} gpu_cmdbuf_t;
```

**Architettura del driver:**

```
include/gpu.h               — syscall numbers + strutture pubbliche
drivers/gpu/gpu.h           — interfaccia interna driver
drivers/gpu/agx.c           — backend Apple AGX (M1/M2/M3/M4)
drivers/gpu/virtio_gpu.c    — backend virtio-GPU per QEMU
drivers/gpu/gpu_sw.c        — software fallback (rasterizzazione CPU)
kernel/gpu_syscall.c        — implementazione syscall 120-133
```

**Differenze chiave rispetto alla pipeline normale (M5b):**
- **M5b** è la pipeline completa del server grafico: compositor, Wayland, scanout tramite server
- **M3-04** sono le syscall di accesso *diretto* al metallo: un task RT può scrivere in un
  command buffer GPU e fare page-flip senza passare dal compositor — latenza ~50µs invece di ~5ms
- Caso d'uso tipico: renderizzatore RT che deve rispettare deadline vsync (16.67ms a 60Hz)

**Dipende da:** M3-01 (syscall dispatcher), M3-02 (mmap per buffer GPU)
**Necessario per:** M5b-01 (il server grafico usa queste syscall internamente)

---

### ✅ M3-03 · ANE Syscall Interface — Apple Neural Engine
**Stato:** COMPLETATA

**Contesto hardware:**
Il chip AI dei processori Apple M-series è l'**ANE** (Apple Neural Engine), un acceleratore
hardware dedicato all'inferenza di reti neurali. Ogni generazione introduce più TOPS:

| Chip | ANE TOPS | Core ANE |
|------|----------|----------|
| M1   | 11 TOPS  | 16 core  |
| M2   | 15.8 TOPS| 16 core  |
| M3   | 18 TOPS  | 16 core  |
| M4   | 38 TOPS  | 16 core  |

L'ANE accetta programmi compilati in formato **`.hwx`** (prodotto dal compilatore
`anecc` / CoreML compiler di Apple). I dati di input/output usano il formato tiled
**HWCX** (Height × Width × Channel × packed) in buffer DMA-coerenti.

**RT design:**
- Submit inferenza **non-blocking**: il task deposita il job e continua — O(1)
- Wait con **deadline fissa**: `ane_wait()` blocca al massimo `timeout_ns` — mai unbounded
- Buffer DMA pre-allocati al boot — zero `kmalloc` nel path di inferenza
- Priorità job ereditata dal task chiamante (priority donation all'ANE scheduler)
- ANE su QEMU: assente — il driver rileva la mancanza e usa un **software fallback**
  (`ane_sw.c`) che esegue l'inferenza sulla CPU via codice C generato dall'offline compiler

**Numeri syscall (range dedicato 100–119 — non collidono con POSIX):**

| Nr  | Nome                    | RT-safe | Firma C                                                          | Note |
|-----|-------------------------|---------|------------------------------------------------------------------|------|
| 100 | `ane_query_caps`        | Sì — O(1)   | `(ane_caps_t *out)`                                          | Legge registro MMIO versione/TOPS, nessun I/O |
| 101 | `ane_buf_alloc`         | No          | `(size_t size, uint32_t flags) → ane_buf_handle_t`           | Alloca buffer DMA-coerente; mai in hot path RT |
| 102 | `ane_buf_free`          | Sì — O(1)   | `(ane_buf_handle_t h)`                                       | Libera da pool pre-allocato |
| 103 | `ane_model_load`        | No          | `(const void *hwx_buf, size_t size) → ane_model_handle_t`   | Valida + carica programma `.hwx` nell'ANE; solo al setup |
| 104 | `ane_model_unload`      | Sì — O(1)   | `(ane_model_handle_t m)`                                     | Rimuove modello dalla cache ANE |
| 105 | `ane_inference_submit`  | **Sì**      | `(ane_model_handle_t m, ane_buf_handle_t in, ane_buf_handle_t out, uint32_t prio) → ane_job_handle_t` | Non-blocking: deposita job nella coda ANE e ritorna subito |
| 106 | `ane_inference_wait`    | **Sì**      | `(ane_job_handle_t j, uint64_t timeout_ns) → int`            | Blocca fino a completamento o timeout; timeout = 0 → polling |
| 107 | `ane_inference_run`     | No          | `(ane_model_handle_t m, ane_buf_handle_t in, ane_buf_handle_t out) → int` | Submit sincrono (= submit + wait senza timeout) — mai da task hard-RT |
| 108 | `ane_job_cancel`        | Sì — O(1)   | `(ane_job_handle_t j) → int`                                 | Annulla job pending nella coda |
| 109 | `ane_job_status`        | Sì — O(1)   | `(ane_job_handle_t j, ane_job_stat_t *out) → int`            | Legge stato job senza bloccare (polling RT-safe) |

**Strutture dati principali:**

```c
/* Capacità hardware ANE */
typedef struct {
    uint32_t  version;          /* ANE HW version (0x30 = M1, 0x40 = M2, ...) */
    uint32_t  num_cores;        /* numero core ANE (sempre 16 su M-series) */
    uint32_t  tops_x100;        /* TOPS × 100 (es. 1100 = 11.0 TOPS) */
    uint32_t  max_model_size;   /* dimensione massima programma .hwx in byte */
    uint32_t  flags;            /* ANE_CAP_SWFALLBACK se su QEMU */
} ane_caps_t;

/* Handle opaco (indice in pool statico — 16 bit) */
typedef uint32_t ane_buf_handle_t;
typedef uint32_t ane_model_handle_t;
typedef uint32_t ane_job_handle_t;

/* Stato di un job in corso */
typedef struct {
    uint32_t  state;            /* ANE_JOB_PENDING / RUNNING / DONE / ERROR */
    uint32_t  error_code;       /* 0 = OK */
    uint64_t  submit_ns;        /* timestamp submit (CNTPCT_EL0) */
    uint64_t  done_ns;          /* timestamp completamento */
    uint64_t  cycles;           /* cicli ANE consumati (profilazione) */
} ane_job_stat_t;
```

**Flags `ane_buf_alloc`:**
- `ANE_BUF_INPUT`  — buffer di input per l'ANE (lettura dalla CPU, DMA read dall'ANE)
- `ANE_BUF_OUTPUT` — buffer di output (scrittura dall'ANE, lettura dalla CPU)
- `ANE_BUF_PINNED` — mai swappato (obbligatorio per task hard-RT)

**Architettura del driver (componenti):**

```
include/ane.h               — syscall numbers + strutture pubbliche
drivers/ane/ane.h           — interfaccia interna driver
drivers/ane/ane_hw.c        — accesso MMIO ANE reale (M1/M2/M3/M4)
drivers/ane/ane_sw.c        — software fallback (QEMU / CPU emulation)
kernel/ane_syscall.c        — implementazione syscall 100-109
```

**Integrazione con lo scheduler:**
- `ane_inference_submit()` assegna al job la priorità del `current_task` → priority inheritance
- L'IRQ di completamento ANE (AIC line dedicata) sblocca i task in attesa via `sched_unblock()`
- `sched_tcb_t` esteso con campo `ane_job_waiting` per fast-path unblock

**Path di sviluppo:**
1. **Stub QEMU** (software fallback): tutte le syscall compilano e funzionano su QEMU
   via `ane_sw.c` (inferenza CPU). Permette lo sviluppo e il testing del layer syscall
   senza hardware reale.
2. **Driver ANE reale** (`ane_hw.c`): MMIO map da device tree Apple, command buffer
   submission, IRQ completion — basato su reverse engineering Asahi Linux (`apple-ane`).
3. **Compilatore offline** (`tools/anecc_wrapper/`): wrapper attorno al compilatore Apple
   (`anecc`) o alternativa open source per produrre file `.hwx` da modelli ONNX/TFLite.

**Implementazione:**
- `include/ane.h` — strutture pubbliche, syscall numbers, flag, `ane_hwx_header_t`
- `drivers/ane/ane_internal.h` — pool interni, `ane_backend_ops_t` vtable
- `drivers/ane/ane_hw.c` — rilevamento Apple Silicon via `MIDR_EL1[31:24]==0x61`; TOPS/versione da PartNum; su QEMU delega a SW
- `drivers/ane/ane_sw.c` — CPU fallback: identità (memcpy) se in/out stessa dimensione, zero-fill altrimenti; timing reale con `timer_now_ns()`
- `kernel/ane_syscall.c` — pool statici (`buf[64]`, `model[16]`, `job[32]`), tutte e 10 le syscall, `ane_init()` che registra 100-109 in `syscall_table`
- Fix timeout: `inference_wait` gestisce `UINT64_MAX` come attesa infinita (per `inference_run`)
- Handle 1-based: 0 / `ANE_INVALID_HANDLE` = invalido

**Dipende da:** M3-01 (syscall dispatcher), M5b-01 (driver MMIO ANE reale)

---

## MILESTONE 4 — Input da Tastiera

### ✅ M4-01 · PL050 PS/2 Keyboard
**Priorità:** ALTA

**RT design:** driver interrupt-driven, zero polling. Ring buffer lock-free (SPSC).

- PL050 @ `0x09050000`, IRQ #17
- Ring buffer 256 byte: produttore = IRQ handler, consumatore = task utente
- `keyboard_getc()` non blocca mai: ritorna 0 se buffer vuoto
- Latenza input → ring buffer: < 10µs (entro la finestra del tick 1ms)

---

### ✅ M4-02 · VirtIO Input
**Priorità:** MEDIA

Alternativa moderna al PS/2 su QEMU `virt`, con backend `virtio-input`
e fallback automatico su UART PL011.

- `virtio-keyboard-device` via `virtio-mmio` (device ID 18)
- Queue `eventq` con split vring statica
- IRQ GIC dinamica: `IRQ_VIRTIO(slot)` ricavata dal MMIO slot trovato
- Conversione `EV_KEY` Linux input → ASCII in ring buffer SPSC 256B
- `keyboard_getc()` resta non-blocking e conserva l'API esistente

---

### ✅ M4-03 · Terminal Line Discipline
**Priorità:** MEDIA

Implementata una line discipline minimale sulla console:

- Echo dei caratteri digitati su UART
- Modalità canonica: `read()` ritorna solo linee complete terminate da newline
- Editing locale con backspace
- `CTRL+C` genera un `SIGINT` minimale sul task foreground della console
- `read()` su console ritorna `-EINTR` quando c'è un interrupt pendente

**Nota:** il sottosistema segnali completo non esiste ancora; la consegna di
`SIGINT` è volutamente minimale e orientata a sbloccare `read()` e shell/task
cooperativi senza introdurre ancora process group o handler utente.

---

### ✅ M4-05 · Mouse Input (VirtIO / PS/2)
**Priorità:** MEDIA

Supporto puntatore per QEMU `virt`, con backend preferito `virtio-mouse-device`
e compatibilità futura con PS/2 mouse dove disponibile.

- Probe `virtio-input` per device pointer/tablet via `virtio-mmio`
- Eventi `EV_REL` / `EV_ABS` + pulsanti `BTN_LEFT/RIGHT/MIDDLE`
- Ring buffer SPSC per eventi mouse (`dx`, `dy`, wheel, button mask)
- API kernel non bloccante: `mouse_get_event()` per consumer grafici
- Integrazione con la boot console grafica: cursore guest, coordinate live,
  stato pulsanti e viewer degli eventi click/move/wheel
- `run-gpu` espone `virtio-mouse-device` e nasconde il cursore host
- Fallback PS/2 lasciato come estensione futura: su QEMU `virt` il path attivo
  e supportato e' VirtIO

**Nota design:** il backend mouse deve riusare il pattern vring/IRQ/cache già
introdotto in M4-02 per limitare la complessità e mantenere WCET prevedibile.

---

### ✅ M4-04 · Supporto Font UTF-8 completo
**Stato:** COMPLETATA

Esteso il sistema di rendering testuale per supportare UTF-8 con Latin-1 Supplement.

**Componenti implementati:**

- **Decoder UTF-8** (`kernel/utf8.c` + `include/utf8.h`):
  - `utf8_decode(const char **s)` — decodifica 1–4 byte, avanza il puntatore, WCET O(1)
  - `utf8_strlen(const char *s)` — conta codepoint (non byte), WCET O(n)
  - Errori → U+FFFD; gestisce overlong, surrogate UTF-16, codepoint > U+10FFFF

- **Font esteso inline** in `drivers/framebuffer.c`:
  - `font_ext[]` — tabella ordinata di 65 codepoint (U+00C0–U+00FF + U+FFFD)
    con bitmaps 8×16 per Latin-1 Supplement completo (accenti Western European)
  - `font_ext_lookup(cp)` — ricerca binaria O(log 65) ≈ O(1) in pratica
  - ASCII (U+0020–U+007E): lookup diretto O(1) nel `font_8x16` esistente

- **API pubblica** (`include/framebuffer.h`):
  - `fb_draw_char_utf8(x, y, codepoint, fg, bg)` — singolo codepoint
  - `fb_draw_string_utf8(x, y, utf8_str, fg, bg)` — stringa UTF-8 completa
  - `fb_draw_string_centered_utf8(utf8_str, fg, bg)` — centrata

**Copertura font:**
  - U+0020–U+007E: ASCII completo (32–126) — O(1)
  - U+00C0–U+00FF: Latin-1 Supplement — lettere accentate IT/FR/DE/ES/PT — O(log N)
  - Sconosciuto: glyph box U+FFFD

**RT design:** `font_ext[]` in `.rodata` (cache-friendly). Nessuna allocazione.
`fb_draw_string_utf8()` non è in hot path RT — solo per output UI.

---

## MILESTONE 5 — Filesystem ext4

### Decisione architetturale · Sostituzione ReiserFS → ext4

Per EnlilOS, `ext4` e' il fit migliore se l'obiettivo non e' un sistema piccolo
embedded ma un OS realtime affidabile e anche general-purpose:

- `ext4`: e' maturo, diffusissimo, con tool di recovery consolidati, journaling,
  extent, `fsck`, semantica POSIX familiare e un ecosistema reale per immagini,
  test, bootstrap e manutenzione.
- `littlefs`: resta eccellente per storage embedded e footprint bounded, ma e'
  pensato esplicitamente per microcontroller e flash piccoli; come filesystem
  principale di un OS general-purpose sarebbe una scelta troppo limitante.
- `XFS`: ottimo per scalabilita' e throughput, ma e' piu' adatto come scelta
  specialistica per grossi volumi/dataset che come filesystem root "default"
  di un microkernel ancora in crescita.
- `F2FS`: interessante su flash, ma cleaning e politiche adattive introducono
  piu' variabilita' e complessita' rispetto al compromesso che ci serve qui.

**Decisione:** sostituire ReiserFS con `ext4` come filesystem persistente di
milestone 5. In ottica RT, i task hard-RT continuano comunque a non fare I/O
diretto: il filesystem vive dietro un server VFS/blk con IPC e timeout.

### Struttura consigliata del sottosistema storage

- Bootstrap: `initrd` / `CPIO` in RAM per il primo userspace e il recovery path
- Persistenza principale: `ext4` su `virtio-blk` come root/data filesystem
- Policy RT: tutte le operazioni disco passano da server dedicati a priorita'
  bassa, mai direttamente dal task hard-RT
- Profilo integrita'/latenza iniziale consigliato per `ext4`:
  `data=ordered`, `nodelalloc`, `commit=1`, `max_batch_time=0`
- Profilo massima integrita' per sottoalberi critici o partizioni critiche:
  `data=journal` dove la latenza extra e' accettabile

### ✅ M5-01 · VirtIO Block Device
**Stato:** COMPLETATA

**RT design:** I/O bloccante mai da task hard-RT; solo da server blk a priorità bassa.

**Implementazione (`drivers/blk.c` + `include/blk.h`):**

- Probe virtio-mmio: scansiona 32 slot, rileva device ID 2 (virtio-blk), versione 2
- Negoziazione feature: `VIRTIO_F_VERSION_1`; nessuna feature blk-specifica richiesta
- Vring split a 16 entry (queue depth = `BLK_QUEUE_DEPTH`), memoria statica 4KB allineata
- **Free list di descriptor** intrusive a 3 slot per richiesta (hdr + data + status):
  alloc O(1), free O(1)
- **`blk_read_sync` / `blk_write_sync`**: I/O sincrono con busy-wait bounded
  (`BLK_POLL_TIMEOUT = 5 000 000 cicli ≈ 80ms a 62.5 MHz`) — usato solo da server blk
- `VRING_AVAIL_F_NO_INTERRUPT` impostato: nessuna IRQ, solo polling
- `bvq_submit()`: flush minimo — solo avail ring e descriptor toccati (non intera pagina)
- Capacity letta dal config space al boot (offset 0 = `uint64_t capacity`)
- Target QEMU: `run-blk` in Makefile con `disk.img` raw 64MB creata da `make disk.img`

**API pubblica:**
- `blk_init()` — rilevamento + init al boot
- `blk_is_ready()` — 1 se pronto
- `blk_sector_count()` — settori totali
- `blk_read_sync(sector, buf, count)` — legge count×512B
- `blk_write_sync(sector, buf, count)` — scrive count×512B

**Codici di errore:** `BLK_OK`, `BLK_ERR_NOT_READY`, `BLK_ERR_IO`, `BLK_ERR_TIMEOUT`, `BLK_ERR_RANGE`, `BLK_ERR_BUSY`

**Dipende da:** M3-01 (niente IPC ancora, server blk sarà M5-02+)
**Necessario per:** M5-02 VFS layer, M5-03 ext4 read path

---

### ✅ M5-02 · VFS Layer
**Priorità:** CRITICA

**RT design:** VFS gira interamente come server user-space (stile Hurd).
Il kernel non sa nulla di filesystem. I task comunicano via IPC.

- `vfs_ops_t`: `open`, `read`, `write`, `readdir`, `stat`, `close`
- Mount table: path → driver (array statico, nessuna allocazione runtime)
- File descriptor table per processo: array fisso di MAX_FD=64
- Mount profile iniziale:
  `/` → `initrd` read-only
  `/sysroot` oppure `/data` → `ext4` su `virtio-blk`

**Implementato ora:** bootstrap VFS in-kernel con mount table statica,
`devfs`, rootfs read-only di bootstrap, mount `/data` e `/sysroot`
preparati quando `virtio-blk` e' pronto, e syscall `open/read/write/close/fstat`
instradate interamente su `vfs_ops_t`.

---

### ⬜ M5-03 · ext4 — Mount & Read Path
**Priorità:** ALTA

Integrazione di `ext4` nel server VFS user-space:
- Parsing superblock, block group descriptors, inode table, extent tree
- Mount read-only iniziale per ridurre il rischio e chiudere il bootstrap
- `open`, `read`, `readdir`, `stat` mappati sul server VFS
- Buffer cache e inode cache statiche/bounded nel server filesystem
- Supporto immagine host tramite `mkfs.ext4`, `e2fsck`, `debugfs`

Tool immagine host iniziali:
- `mkfs.ext4 disk.img`
- `e2fsck -f disk.img`
- Layout consigliato dev: blocchi 4KB, immagine raw montata via `virtio-blk`

---

### ⬜ M5-04 · ext4 — Write Path & Sync Policy
**Priorità:** MEDIA

- `write`, `mkdir`, `unlink`, `rename`, `truncate`
- Journal replay al mount e supporto writeback del journal
- Politica esplicita di `sync` / `fsync` per confinare la latenza nei punti noti
- Flush su deadline-safe server thread, mai nel task hard-RT chiamante
- Policy di mount per dev/prod:
  dev → `data=ordered,nodelalloc,commit=1,max_batch_time=0`
  critical → valutare `data=journal`
- Test crash-consistency usando power-cut simulato sull'immagine raw

---

### ⬜ M5-05 · initrd / Ramdisk Bootstrap
**Priorità:** ALTA (prima di ext4 r/w)

Formato CPIO: montato come `/` al boot, contiene il primo ELF da eseguire.
Tutto in RAM → latenza di accesso O(1), adatto a task RT che leggono config al boot.

---

## MILESTONE 5b — GPU & Display (Apple M-series Metal-compatible)

> **Principio:** tutta la pipeline grafica (framebuffer, compositing, rendering 2D/3D)
> passa **esclusivamente** attraverso il chip grafico integrato del processore M-series.
> Il kernel non scrive mai pixel direttamente in RAM — usa il GPU per ogni operazione
> di output visivo. Questo garantisce DMA coerenza, banda ottimale e zero CPU stall
> sulle operazioni grafiche.
>
> **Nota su QEMU:** in ambiente di sviluppo si usa `virtio-gpu` o `ramfb` come proxy;
> le API del driver GPU sono progettate per essere sostituibili senza cambiare il codice
> dei client (server grafico, compositor).

---

### ✅ M5b-01 · GPU Driver — Virtio-GPU (dev) / Apple AGX (target)
**Priorità:** ALTA

**Architettura:**
- Il kernel espone un'interfaccia `gpu_ops_t` astratta (stile `vfs_ops_t`)
- In sviluppo: backend `virtio-gpu` su QEMU (`-device virtio-gpu-pci`)
- Su M-series reale: backend **Apple AGX GPU** via MMIO (compatibile con driver Asahi Linux)
- Nessun accesso diretto al framebuffer dalla CPU dopo l'init — tutto passa per command buffer GPU

**Componenti:**
- `drivers/gpu/virtio_gpu.c` — implementazione virtio-gpu per QEMU
- `drivers/gpu/agx.c` — stub Apple AGX (M1/M2/M3), MMIO map dal device tree
- `include/gpu.h` — interfaccia astratta `gpu_ops_t`

**Operazioni GPU esposte:**
```c
gpu_ops_t {
    int  (*init)(void);
    void (*flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void (*blit)(gpu_buf_t *src, gpu_buf_t *dst, gpu_rect_t *r);
    void (*fill_rect)(gpu_buf_t *dst, gpu_rect_t *r, uint32_t color);
    void (*draw_text)(gpu_buf_t *dst, uint32_t x, uint32_t y,
                      const char *str, uint32_t fg, uint32_t bg);
}
```

**RT design:**
- Command buffer DMA pre-allocato al boot (nessuna allocazione in hot path)
- `gpu_flush()` asincrono con completion callback → non blocca il core RT
- Priorità IRQ GPU = `GIC_PRIO_DRIVER` (non interrompe task hard-RT)

---

### ⬜ M5b-02 · Scanout & Display Engine
**Priorità:** ALTA (dipende da M5b-01)

La GPU gestisce il **display engine** — il kernel non mantiene mai un framebuffer
software accessibile dalla CPU in modalità operativa:

- **Scanout buffer** allocato in memoria GPU-accessibile (IOMMU mappato)
- **Page flip** atomico: il compositor prepara il frame in un back buffer,
  poi flip — il display engine commuta senza tearing
- **Vsync IRQ**: segnalato dalla GPU al GIC, sveglia il compositor con latenza < 1ms
- Risoluzione dinamica: il driver negozia con il display (`EDID` o `Display Stream Compression`)

**Integrazione con il server grafico (M7-02):**
- Il server grafico (user-space, stile Wayland) scrive in surface buffer GPU
- Passa il buffer al compositor via IPC con zero-copy (shared GPU memory handle)
- Il compositor chiama `gpu_flush()` → scanout → display

---

### ⬜ M5b-03 · GPU Memory Manager
**Priorità:** MEDIA (dipende da M5b-01)

Gestione della VRAM / memoria GPU-mappata:

- Pool di **GPU buffer objects** (GBO) pre-allocati al boot
- `gpu_alloc(size, type)`: O(1) da pool, tipi: `GPU_MEM_SCANOUT`, `GPU_MEM_TEXTURE`, `GPU_MEM_CMD`
- `gpu_free(gbo)`: O(1) — ritorno al pool
- **IOMMU mapping** per DMA sicuro: la GPU vede solo le sue regioni autorizzate
- Cache management: `gpu_flush_cache(gbo)` prima di submit command buffer

---

### ⬜ M5b-04 · 2D Rendering Accelerato
**Priorità:** MEDIA (dipende da M5b-02)

Operazioni 2D eseguite sulla GPU, non sulla CPU:

- `gpu_fill_rect()` — riempimento rettangolo (sfondo desktop, finestre)
- `gpu_blit()` — copia buffer accelerata (scroll, finestre trasparenti)
- `gpu_draw_glyph()` — rendering font bitmap/vettoriale via GPU shader semplice
- `gpu_alpha_blend()` — compositing con canale alpha (trasparenza finestre)

Tutti i comandi vengono accodati in un **command ring** e sottomessi in batch
per massimizzare il throughput GPU e minimizzare il numero di context switch GPU.

---

### ⬜ M6-01 · ELF64 Static Loader
**Priorità:** CRITICA

**RT design:** ELF loading è operazione di setup (non hot path). Tuttavia:
- I segmenti PT_LOAD vengono **pre-faultati** dopo il caricamento (lettura pagina per pagina)
  per eliminare page fault futuri durante l'esecuzione RT
- Stack pre-committato interamente (no guard page + stack growth per task RT)

- Parsing header: magic `0x7F454C46`, `e_machine = 0xB7` (AArch64)
- Mapping segmenti PT_LOAD con permessi corretti (R/W/X)
- Stack utente 8MB pre-allocato a `0x7FFF000000`
- ABI stack: `argc`, `argv[]`, `envp[]`, `auxv[]` (AT_PHDR, AT_ENTRY, AT_PAGESZ, AT_HWCAP)

---

### ⬜ M6-02 · execve() completo
**Priorità:** ALTA
VFS open → verifica ELF → carica segmenti → drop a EL0.

---

### ⬜ M6-03 · ELF Dynamic Loader
**Priorità:** BASSA
Solo dopo static loader funzionante e testato.

---

## MILESTONE 7 — RT IPC

### ⬜ M7-01 · IPC Sincrono a Latenza Fissa
**Priorità:** ALTA — cuore del microkernel RT

L'IPC attuale è asincrono con ring buffer. Per RT serve IPC sincrono con:
- **Rendez-vous** (stile L4): il mittente blocca finché il ricevitore accetta
- **Priority donation:** il task bloccato in attesa di risposta da un server
  "dona" la sua priorità al server per la durata della chiamata
  → previene priority inversion sull'IPC
- **Bounded latency:** il kernel garantisce che `ipc_call()` consegni il messaggio
  entro N cicli (misurabile, configurabile per ogni canale)
- **Zero-copy per messaggi piccoli** (≤64 byte via registri, no buffer intermedio)

---

### ⬜ M7-02 · Console e Shell Minimale (`nsh`)
**Priorità:** MEDIA

Text mode 80×25 su framebuffer. Shell ELF statico con comandi: `ls`, `cat`, `echo`,
`exec`, `clear`, `top` (mostra task attivi e loro utilizzo CPU/deadline).

---

## Dipendenze

```
M1-01 ✅ → M1-02 ✅
M1-02 → M1-03 ✅ → M1-04 ✅
M1-03 → M2-01 ✅ → M2-02 → M2-03
M2-03 → M3-01 → M3-02
M3-02 → M4-01
M3-01 → M3-04 (GPU syscall)
M3-01 → M3-03 (ANE syscall)
M3-02 → M3-04 (mmap per GPU buffer)
M3-04 → M5b-01 (server grafico usa GPU syscall internamente)
M3-03 → M5b-01 (pattern MMIO/DMA condiviso)
M3-02 → M5-01 → M5-02 → M5-03/M5-04
         M5-05 ──────────────────────┐
M5-01 → M5b-01 → M5b-02 → M5b-03   │
                 M5b-04 ─────────────┤
M3-02 → M6-01 ←─────────────────────┘
M6-01 → M6-02 → M7-02
M2-03 → M7-01
```

## Prossimi tre step consigliati

1. **M5-01** VirtIO Block — storage base per VFS — 3-4 ore
2. **M5b-02** Scanout & Display Engine — page-flip vsync-aware — 2-3 ore
3. **M4-02** VirtIO Input — tastiera moderna (richiede pattern vring già fatto) — 2 ore

Dopo M4-01 le syscall read/write sono fully functional con tastiera reale.
Dopo M3-04 un task RT può scrivere direttamente in un command buffer GPU con latenza ~50µs.
Dopo M5b-01 tutta la grafica transita dalla GPU — zero accessi CPU al framebuffer.
