# NROS — Backlog Implementazioni
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

### ⬜ M2-02 · ARM Generic Timer
**Priorità:** CRITICA — pilastro del sistema RT

**RT design:** il timer è l'unico elemento che garantisce i deadline.

- Frequenza letta da `CNTFRQ_EL0` (tipicamente 62.5 MHz su QEMU)
- Tick del sistema: **1ms** (1000Hz) per granularità sufficiente a task con deadline 5ms+
- `get_time_ns()` — lettura diretta di `CNTP_TVAL_EL0`, O(1), nessuna lock
- **Time accounting per task:** ogni task ha un `budget_ns` (tempo CPU rimanente nel periodo)
- **Overrun detection:** se un task usa più del suo budget → log + preemption immediata

---

### ⬜ M2-03 · Scheduler Real-Time
**Priorità:** CRITICA

**RT design:** non Round-Robin. Scelta tra due policy:

**Fixed-Priority Preemptive (FPP)** — più semplice, comune in sistemi embedded RT:
- 256 livelli di priorità (come VxWorks/FreeRTOS)
- Bit-mask a 256 bit per trovare la priorità più alta in O(1) (4 operazioni `CLZ`)
- Preemption immediata: se task con priorità > corrente diventa READY → switch istantaneo

**Earliest Deadline First (EDF)** — ottimale per utilizzo CPU:
- Task ordinati per deadline assoluta
- `pick_next()` = O(log N) con min-heap
- Necessita deadline note a compile-time o al momento della creazione del task

**Scelta per NROS:** FPP come policy base + EDF opzionale per task periodici dichiarati.

- Context switch: salva x0-x30 + registri sistema, carica TTBR0 del nuovo task
- **Latenza context switch target:** < 500ns (≈30 cicli registri + TLB flush ASID)
- `schedule()` invocabile da qualsiasi contesto kernel (preemptible kernel)

---

## MILESTONE 3 — Syscall Interface

### ⬜ M3-01 · Syscall Dispatcher
**Priorità:** CRITICA

**RT design:** tabella syscall indicizzata, nessuna ricerca. Ogni syscall ha una
classe di priorità che influenza il suo scheduling (syscall RT non possono essere
preemptate da syscall non-RT).

- Handler per `EC = 0x15` (SVC #0) nella vector table
- Numero syscall in `x8`, argomenti `x0-x5`, ritorno `x0`
- Syscall marcate RT/non-RT: le RT completano con priorità elevata

---

### ⬜ M3-02 · Syscall Base (13 syscall)

| Nr | Nome | RT-safe | Note |
|----|------|---------|------|
| 1 | `write` | Sì (buffered) | fd=1 → console, non blocca |
| 2 | `read` | Sì (non-blocking) | fd=0 → keyboard, ritorna -EAGAIN se vuoto |
| 3 | `exit` | Sì | Termina task, libera risorse |
| 4 | `open` | No (I/O filesystem) | Non chiamare da task hard-RT |
| 5 | `close` | Sì | |
| 6 | `fstat` | No | |
| 7 | `mmap` | No (alloca pagine) | Pre-allocare memoria al boot per task RT |
| 8 | `munmap` | No | |
| 9 | `brk` | No | |
| 10 | `execve` | No | |
| 11 | `fork` | No | |
| 12 | `waitpid` | Sì (timed) | Con timeout obbligatorio per RT |
| 13 | `clock_gettime` | Sì — O(1) | Lettura diretta `CNTP_TVAL` |

---

## MILESTONE 4 — Input da Tastiera

### ⬜ M4-01 · PL050 PS/2 Keyboard
**Priorità:** ALTA

**RT design:** driver interrupt-driven, zero polling. Ring buffer lock-free (SPSC).

- PL050 @ `0x09050000`, IRQ #17
- Ring buffer 256 byte: produttore = IRQ handler, consumatore = task utente
- `keyboard_getc()` non blocca mai: ritorna 0 se buffer vuoto
- Latenza input → ring buffer: < 10µs (entro la finestra del tick 1ms)

---

### ⬜ M4-02 · VirtIO Input
**Priorità:** MEDIA
Alternativa moderna al PS/2. Necessita VirtIO queue (vring) — implementare dopo M5-01.

---

### ⬜ M4-03 · Terminal Line Discipline
**Priorità:** MEDIA
Echo, modalità canonica, backspace, CTRL+C → SIGINT. Non in hot path RT.

---

## MILESTONE 5 — Filesystem ReiserFS

### ⬜ M5-01 · VirtIO Block Device
**Priorità:** CRITICA (prerequisito ReiserFS)

**RT design:** I/O bloccante è incompatibile con task hard-RT. Soluzione:
- Task hard-RT **non** accedono mai al filesystem direttamente
- I/O su disco gestito da un server dedicato a priorità BASSA
- Task RT comunicano col server via IPC con timeout: se il server non risponde
  entro il deadline → il task RT continua senza i dati (graceful degradation)

- VirtIO-blk: `-drive format=raw,file=disk.img -device virtio-blk-device`
- Virtqueue con split ring, DMA asincrono
- Buffer cache: 64 blocchi × 4KB = 256KB, LRU, gestita dal server blk (non nel kernel)

---

### ⬜ M5-02 · VFS Layer
**Priorità:** CRITICA

**RT design:** VFS gira interamente come server user-space (stile Hurd).
Il kernel non sa nulla di filesystem. I task comunicano via IPC.

- `vfs_ops_t`: `open`, `read`, `write`, `readdir`, `stat`, `close`
- Mount table: path → driver (array statico, nessuna allocazione runtime)
- File descriptor table per processo: array fisso di MAX_FD=64

---

### ⬜ M5-03 · ReiserFS v3 — Read-Only
**Priorità:** ALTA

Parsing del formato ReiserFS v3.3:
- Superblock (offset 64KB): magic `0x1BADA1`, block size, root block
- B-tree (S+tree): chiave `(dir_id, obj_id, offset, type)`
- Item types: `DIRENTRY`, `STAT_DATA`, `INDIRECT`, `DIRECT` (dati inline)
- `reiserfs_open(path)` → ricerca B-tree per nome
- `reiserfs_read(inode, offset, buf, len)` → lettura blocchi dati

Tool di creazione immagine (host): `mkreiserfs disk.img`

---

### ⬜ M5-04 · ReiserFS v3 — Read-Write
**Priorità:** MEDIA
Modifica B-tree, allocazione blocchi, replay journal al mount.

---

### ⬜ M5-05 · initrd / Ramdisk Bootstrap
**Priorità:** ALTA (prima di ReiserFS r/w)

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

### ⬜ M5b-01 · GPU Driver — Virtio-GPU (dev) / Apple AGX (target)
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
M3-02 → M5-01 → M5-02 → M5-03/M5-04
         M5-05 ──────────────────────┐
M5-01 → M5b-01 → M5b-02 → M5b-03   │
                 M5b-04 ─────────────┤
M3-02 → M6-01 ←─────────────────────┘
M6-01 → M6-02 → M7-02
M2-03 → M7-01
```

## Prossimi tre step consigliati

1. **M2-02** ARM Generic Timer (1ms tick) — 1-2 ore
2. **M2-03** Scheduler FPP a 256 priorità — 3-4 ore
3. **M5b-01** GPU driver (virtio-gpu su QEMU, stub AGX) — 3-4 ore

Dopo M2-02 + M2-03 il sistema è un microkernel RT preemptivo funzionante.
Dopo M5b-01 tutta la grafica transita dalla GPU — zero accessi CPU al framebuffer.
