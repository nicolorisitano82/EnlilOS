# Studio: EDF vs Neural-Assisted Scheduler — Coesistenza in EnlilOS

**Data:** 2026-04-07  
**Autore:** studio architetturale per EnlilOS  
**Milestone di riferimento:** M13-01 (EDF), M16-NAS (NAS proposto)

---

## 1. Punto di partenza: lo scheduler FPP attuale

EnlilOS usa un **Fixed Priority Preemptive (FPP)** scheduler con:

- `rq_head[256]` / `rq_tail[256]` — FIFO per priorità, O(1) pick-next via `bitmap_find_first()`
- TCB 64 byte esatti, layout fisso (vincolo `sched_switch.S`)
- Priority Inheritance via `sched_task_donate_priority()` — mai mutazione diretta
- Campi già presenti nel TCB: `period_ms`, `deadline_ms`, `budget_ns`, `runtime_ns`

Il TCB ha già i campi per EDF e CBS. La struttura dati manca: l'heap.

---

## 2. EDF — Earliest Deadline First (M13-01)

### 2.1 Teoria

EDF è un algoritmo di scheduling **ottimale** per sistemi uniprocessore: se un insieme
di task è schedulabile da qualunque algoritmo, è schedulabile anche da EDF.

Condizione di schedulabilità (Liu & Layland 1973):

```
Σ (Ci / Ti) ≤ 1
```

dove `Ci` = WCET del task i, `Ti` = periodo. FPP (RMS) ha bound `n*(2^(1/n)−1) ≈ 0.693`
per n→∞; EDF arriva fino a **1.0** — usa il 100% della CPU senza miss.

### 2.2 Struttura dati: min-heap su `deadline_abs_ns`

```
                    edf_heap[0]   ← deadline minima = prossimo task
                   /             \
           edf_heap[1]         edf_heap[2]
          /          \
    edf_heap[3]   edf_heap[4]
```

- **Insert**: O(log N) — percolate-up
- **Extract-min**: O(log N) — sostituisce root con last, percolate-down
- **N = 32** (SCHED_MAX_TASKS): O(log 32) = **5 confronti** — completamente trascurabile

Campo chiave: `deadline_abs_ns` in unità ns. Il TCB attuale ha `deadline_ms`
(in jiffies ms) — va esteso a 64-bit ns per precisione sub-millisecondo.

### 2.3 Comportamento con task aperiodici: CBS

I task non-RT (NSH, blkd, vfsd) non hanno deadline naturale.
Il **Constant Bandwidth Server (CBS)** assegna loro un budget artificiale:

```
CBS: ogni task aperiodico ottiene un "server" (Cs, Ts)
    - quando il task esegue, scala Cs
    - quando Cs si esaurisce: deadline_abs = timer_now_ns() + Ts
    - il task viene reinserito nell'heap con la nuova deadline posticipata
```

Questo garantisce che i task aperiodici non rubino CPU ai task RT
anche sotto EDF (che non distingue naturalmente priorità).

### 2.4 Cosa cambia nel kernel per M13-01

| Componente | Modifica |
|-----------|---------|
| `include/sched.h` | `deadline_ms` → `deadline_abs_ns` (uint64_t ns); `SCHED_MODE_FPP / SCHED_MODE_EDF` |
| `kernel/sched.c` | `edf_heap[32]`, `edf_insert()`, `edf_extract_min()`, `sched_pick_next_edf()` |
| `kernel/sched.c` | `sched_task_set_deadline(t, period_ns, wcet_ns)` — ammissione + test RMS |
| `kernel/sched.c` | `cbs_server_t cbs[32]` — uno per task aperiodico |
| `kernel/main.c` | `sched_mode` configurabile (cmdline o flag boot) |
| `kernel/selftest.c` | `edf-core` — task periodici con deadline, verifica no miss |

**Impatto sul fast path**: `sched_pick_next_edf()` fa un `edf_extract_min()` —
5 confronti su 32 entry. Ordine di grandezza uguale al `bitmap_find_first()` FPP.
**Nessun impatto sulla latenza IRQ.**

### 2.5 Limiti di EDF

| Limite | Impatto in EnlilOS |
|--------|-------------------|
| **Domino failure**: se un task manca la deadline, tutti quelli dopo soffrono | Senza WCET misurato, `Ci` è una stima manuale → rischio sovrastima |
| **Nessuna priorità intrinseca**: due task con uguale deadline sono indistinguibili | Serve tie-breaking (es. PID, poi priorità base) |
| **Ammissione conservativa**: test Liu&Layland rifiuta carichi > 69% su FPP → 100% su EDF | Con EDF si recupera il 30% di headroom |
| **CBS overhead**: task aperiodici richiedono aggiornamento deadline a ogni periodo | O(log N) per insert — accettabile |
| **Non adattivo**: WCET stimato a design time, non misurato a runtime | Se il comportamento cambia (es. ext4 lento), le stime diventano pessimistiche |

---

## 3. NAS — Neural-Assisted Scheduler

### 3.1 Idea fondamentale

Il NAS non è un algoritmo di scheduling: è un **layer adattivo** che osserva
il comportamento reale dei task e suggerisce aggiustamenti alle loro priorità/budget
senza mai entrare nel fast path.

```
Modello: TCN leggera (~8K parametri float16)
Input:   feature vector per-task (runtime, block rate, ipc rate, deadline miss rate, ...)
Output:  Δprio, Δquantum, Δbudget — applicati via sched_task_donate_priority()
```

Il NAS gira come task kernel a `PRIO_LOW=200`, campiona ogni 100ms,
sottomette un batch di 32 task all'ANE, riceve output, applica guardrail.

### 3.2 Cosa il NAS fa bene

| Capacità | Esempio concreto |
|---------|-----------------|
| **Adattività a runtime** | blkd diventa improvvisamente CPU-intensive → NAS alza la priorità temporaneamente |
| **Stima WCET implicita** | Osserva la distribuzione di `runtime_ns` per periodo → stima automatica di Ci |
| **Rilevamento anomalie** | Task che non si comporta come previsto → confidence bassa → suggerimento scartato |
| **Ottimizzazione globale** | Minimizza miss rate su tutti i task simultaneamente |
| **Self-tuning** | Non richiede parametri manuali per nuovi task |

### 3.3 Cosa il NAS NON può garantire

| Mancanza | Impatto |
|---------|---------|
| **Nessuna garanzia formale** | Non può certificare "questo task non mancherà mai la deadline" |
| **Feedback loop instabile** | Se il modello è mal addestrato, può peggiorare il sistema |
| **Avvio a freddo** | Primi 300ms: nessuna storia → nessun suggerimento |
| **Non certificabile** | Sistemi safety-critical (DO-178C, IEC 61508) non accettano ML in loop chiuso |
| **Dipendenza da ANE** | Se l'ANE è assente o buggy, il NAS non funziona |

---

## 4. Confronto diretto

| Dimensione | FPP (attuale) | EDF (M13-01) | NAS (proposto) |
|-----------|-------------|-------------|----------------|
| **Garanzia schedulabilità** | Bound 69% utilization | Bound 100% — ottimale | Nessuna formale |
| **Complessità pick-next** | O(1) bitmap | O(log N) heap | N/A (non nel fast path) |
| **Latenza IRQ** | < 1µs | < 1µs | N/A |
| **Parametri richiesti** | Priorità base | period + WCET | Nessuno (auto) |
| **Adattività** | Zero | Zero | Alta |
| **Certificabilità** | Sì (analisi statica) | Sì (test RMS) | No |
| **Domino failure** | No (priorità fisse) | Sì (se WCET sbagliato) | Parziale (guardrail) |
| **Overhead runtime** | Minimo | +5 confronti | +100ms batch inference |
| **Uso CPU NPU** | Zero | Zero | ~1% NPU ogni 100ms |
| **Task aperiodici** | FIFO dentro priorità | CBS (budget artificiale) | Gestiti automaticamente |
| **SMP-friendly** | Sì (per-CPU bitmap) | Complesso (global heap) | Sì (per-task suggerimenti) |

### 4.1 Analisi della complementarità

EDF e NAS operano su **dimensioni ortogonali**:

```
EDF:  "chi esegue ADESSO?"           → algoritmo di selezione determinista
NAS:  "quanto peso dare a ciascuno?" → advisor adattivo sui parametri
```

Non si escludono: il NAS può adattare i parametri che EDF usa per prendere le decisioni.

---

## 5. Architettura unificata: EDF + NAS insieme

### 5.1 Visione d'insieme

```
┌─────────────────────────────────────────────────────────────────────┐
│                   Fast Path (IRQ-safe, < 1µs)                       │
│                                                                     │
│   Timer IRQ → sched_tick()                                          │
│       │                                                             │
│       ├─ SCHED_FPP: bitmap_find_first() → O(1)                     │
│       └─ SCHED_EDF: edf_extract_min()  → O(log 32)                 │
│                                                                     │
│   Parametri usati: deadline_abs_ns, priority, ticks_left            │
│         ↑                   ↑                 ↑                     │
│         │ NAS aggiorna fuori dal fast path    │                     │
└─────────┼───────────────────┼─────────────────┼─────────────────────┘
          │                   │                 │
┌─────────┼───────────────────┼─────────────────┼─────────────────────┐
│         │     NAS Task (PRIO_LOW=200)          │                     │
│         │                                     │                     │
│  ┌──────┴──────────────────────────────────────┴───────┐            │
│  │  Profiler Buffer (ring buffer lockless, per-task)    │            │
│  │  runtime_ns, block_count, ipc_calls, miss_deadline  │            │
│  └──────────────────────────┬───────────────────────────┘            │
│                             │                                        │
│  ┌──────────────────────────▼───────────────────────────┐            │
│  │  TCN inference (ANE, 100ms batch)                    │            │
│  │  Output: Δprio, Δdeadline_slack, Δbudget, confidence │            │
│  └──────────────────────────┬───────────────────────────┘            │
│                             │                                        │
│  ┌──────────────────────────▼───────────────────────────┐            │
│  │  Guardrail layer                                     │            │
│  │  - TCB_FLAG_RT: deadline mai posticipata             │            │
│  │  - confidence < soglia: suggerimento scartato        │            │
│  │  - Δdeadline clamped a ±10% del periodo              │            │
│  │  - Usa sched_task_donate_priority() — mai mutazione  │            │
│  └──────────────────────────────────────────────────────┘            │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.2 Come NAS lavora CON EDF

In modalità `SCHED_EDF`, il NAS non tocca le priorità (che EDF ignora).
Agisce invece su:

**a) `deadline_slack_ns`** — finestra di flessibilità sulla deadline

```
deadline_abs_ns effettiva = deadline_hard_ns - slack_ns
```

Il NAS può stringere (`slack = 0`) o allargare (`slack > 0`) la deadline percepita
dall'heap EDF senza mai posticipare la deadline reale del task.

Effetto: un task che il NAS vede "storicamente veloce" ottiene `slack` maggiore
→ EDF lo schedula un po' più tardi → altri task urgenti ottengono CPU prima.

**b) `wcet_estimate_ns`** — stima WCET automatica

```c
/* NAS aggiorna la stima WCET osservando la distribuzione di runtime_ns */
t->wcet_estimate_ns = ema_99th_percentile(prof->runtime_samples, 0.95f);
```

Questo aggiorna il test di ammissione CBS per i task aperiodici:
invece di un Ci statico configurato a mano, il valore è appreso.

**c) CBS budget** — per task aperiodici

Il NAS può aumentare o ridurre il budget CBS di blkd/vfsd/nsh in base
al carico osservato, senza mai interferire con i task RT con deadline esplicita.

### 5.3 Separazione formale delle responsabilità

```
┌─────────────────────────────────────────────────────────┐
│ DOMINIO EDF (deterministico, certificabile)             │
│                                                         │
│  Task RT (FLAG_RT):                                     │
│    - deadline_abs_ns: impostata da sched_task_set_rt()  │
│    - WCET: dichiarato staticamente all'ammissione       │
│    - NAS: può solo osservare (confidence gate blocca)   │
│    - Garanzia: test RMS passa → nessun miss formale     │
└──────────────────────────────┬──────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────┐
│ DOMINIO NAS (adattivo, best-effort)                     │
│                                                         │
│  Task soft-RT e aperiodici (FLAG_USER, non FLAG_RT):    │
│    - WCET: stimato dal NAS (EMA 95° percentile)         │
│    - CBS budget: adattato dal NAS                       │
│    - deadline_slack: suggerita dal NAS                  │
│    - Garanzia: nessuna formale, ma miss rate ridotto     │
└─────────────────────────────────────────────────────────┘
```

### 5.4 Interazione con FPP

In modalità `SCHED_FPP`, il NAS funziona esattamente come descritto nello studio
precedente: aggiusta le priorità donate di task soft-RT senza toccare quelli FLAG_RT.
L'architettura NAS è indipendente dalla politica di scheduling.

---

## 6. Modi operativi (configurabili a boot)

| Modalità | Politica | NAS attivo | Garanzie RT | Adattività |
|---------|---------|-----------|------------|-----------|
| `SCHED_FPP` | Priorità fissa | No | Sì (se prio assegnate bene) | Zero |
| `SCHED_FPP_NAS` | Priorità fissa + advisor | Sì | Sì (guardrail) | Media |
| `SCHED_EDF` | Deadline assoluta | No | Sì (test RMS) | Zero |
| `SCHED_EDF_NAS` | Deadline + WCET adattivo | Sì | Sì (hard RT protetti) | Alta |

Configurazione via flag in `kernel/main.c`:

```c
#define SCHED_MODE_FPP     0
#define SCHED_MODE_FPP_NAS 1
#define SCHED_MODE_EDF     2
#define SCHED_MODE_EDF_NAS 3

static uint32_t sched_mode = SCHED_MODE_FPP;  /* default: retrocompatibile */
```

---

## 7. Sequenza di implementazione

### Fase 1 — EDF puro (M13-01)

Dipendenza: M2-03 (già completata). Può iniziare subito dopo M13-02 (SMP).

```
M13-01a  Min-heap EDF (edf_heap[32], insert/extract O(log N))
M13-01b  deadline_ms → deadline_abs_ns nel TCB
          ⚠ ATTENZIONE: TCB è esattamente 64 byte — deadline_ms è già uint64_t
             alla offset 40. Rinomina e cambia unità: nessun impatto sul layout.
M13-01c  CBS per task aperiodici (budget Cs, periodo Ts, rinnovo deadline)
M13-01d  Test schedulabilità RMS all'ammissione (sched_task_set_rt())
M13-01e  SCHED_MODE_EDF configurabile, default SCHED_FPP
M13-01f  Selftest edf-core: 3 task periodici, verifica zero miss in 1s
```

### Fase 2 — Profiler base (prerequisito NAS)

```
M16-NAS-01  sched_prof.c: ring buffer lockless, campionamento in sched_tick()
             ← overhead: 1 store atomico per tick, nessun lock
M16-NAS-02  API: prof_drain() restituisce snapshot per-task aggregato su N tick
```

### Fase 3 — NAS su FPP (M16-NAS-FPP)

```
M16-NAS-03  NAS task (PRIO_LOW=200), loop 100ms
M16-NAS-04  Feature extractor (16 float16 per task)
M16-NAS-05  Modello TCN embedded (weights come array C, ~16KB)
M16-NAS-06  Inference su ANE (DMA submit/wait) o SW fallback (NEON)
M16-NAS-07  Guardrail + apply via sched_task_donate_priority()
M16-NAS-08  Selftest nas-core: verifica profiler, confidence gate, no RT degradation
```

### Fase 4 — NAS su EDF (M16-NAS-EDF) — richiede M13-01 + M16-NAS-FPP

```
M16-NAS-09  deadline_slack_ns nel TCB (campo aggiuntivo — richiede TCB > 64 byte
             OPPURE si usa budget_ns già presente come slack proxy)
M16-NAS-10  wcet_estimate_ns adattivo via EMA 95° percentile
M16-NAS-11  CBS budget adattivo per task aperiodici
M16-NAS-12  Training pipeline offline: trace QEMU → labels → modello EDF-aware
```

### Dipendenze complete

```
M2-03 (FPP) ──► M13-01 (EDF) ──┐
                                 ├──► M16-NAS-EDF
M2-03 (FPP) ──► M16-NAS-FPP ──┘
                     │
                     └──► M16-NAS-08 (selftest) → training pipeline offline
```

---

## 8. Impatto sul TCB (vincolo critico 64 byte)

Il TCB di EnlilOS è **esattamente 64 byte** per entrare in `task_cache`.
`sched_switch.S` dipende da `sp` a offset 0. Il layout attuale:

```
offset  0: sp            8 byte
offset  8: pid           4 byte
offset 12: priority      1 byte
offset 13: state         1 byte
offset 14: flags         1 byte
offset 15: ticks_left    1 byte
offset 16: runtime_ns    8 byte
offset 24: budget_ns     8 byte  ← usato come CBS budget / slack proxy
offset 32: period_ms     8 byte
offset 40: deadline_ms   8 byte  ← rinominare deadline_abs_ns, cambiare unità
offset 48: *name         8 byte
offset 56: *next         8 byte
TOTALE:                 64 byte
```

**Buona notizia**: `deadline_ms` e `budget_ns` sono già nel TCB.
Per EDF+NAS si può usare `budget_ns` come `deadline_slack_ns` senza aggiungere campi.
Solo il nome cambia semantica in modalità EDF — costo zero sul layout.

Se in futuro servono più campi (es. `wcet_estimate_ns` separato):
opzione A: `task_cache` a 128 byte (cambia solo la cache slab, non `sched_switch.S`),
opzione B: TCB esteso puntato da `*next` (overlay pattern — complesso).

---

## 9. Analisi del rischio combinato

### 9.1 Rischi di EDF

| Rischio | Mitigazione |
|---------|-------------|
| WCET sovrastimato → utilization artificialmente alta | CBS + NAS stima adattiva |
| WCET sottostimato → miss garantita | Test RMS al boot; log warning se utilization > 95% |
| Domino failure su overload | Admission control: rifiuta task se Σ(Ci/Ti) > 0.95 |
| SMP: heap globale = collo di bottiglia | In M13-02: per-CPU heap con work stealing (rinviato) |

### 9.2 Rischi di NAS

| Rischio | Mitigazione |
|---------|-------------|
| Modello mal addestrato | Confidence gate; fallback a EDF/FPP puro se confidence < soglia globale |
| ANE assente (HW diverso) | SW fallback su NEON; NAS disabilitato se ANE non disponibile |
| Feedback loop instabile | Reset donation ogni 500ms; NAS opera solo su task non-FLAG_RT |
| Training dataset non rappresentativo | Raccolta trace su workload reali; CI/CD con trace regression |

### 9.3 Scenario peggiore accettabile

Se NAS produce output errati → il guardrail blocca tutto (confidence gate).
Lo scheduler torna a funzionare come EDF puro (o FPP puro).
Il sistema è **fail-safe by design**: NAS è un advisor, non un controller.

---

## 10. Confronto con lavori correlati

| Sistema | Scheduler base | ML layer | Garanzie RT | Pubblicazione |
|---------|---------------|---------|------------|--------------|
| **Linux CFS** | vruntime tree | No | Nessuna formale | — |
| **LITMUS^RT** | EDF / G-EDF | No | Sì (analisi) | UNC Chapel Hill |
| **Paragon** | FCFS | LSTM (offline) | No | ASPLOS 2013 |
| **Lergan** | EDF | DNN (online) | Parziale | RTSS 2021 |
| **Amp** (Google) | schedutil | GBM trees | No | EuroSys 2023 |
| **EnlilOS EDF+NAS** | EDF+CBS | TCN (ANE) | Sì (hard RT) | — |

La differenza chiave rispetto a Lergan (il lavoro più simile): EnlilOS separa
nettamente il dominio hard-RT (EDF deterministico, nessun NAS) dal dominio
soft-RT/aperiodico (NAS pieno controllo). Lergan usa ML anche per task hard-RT,
con garanzie più deboli.

---

## 11. Conclusione e raccomandazione

### Cosa implementare e in quale ordine

1. **M13-01 EDF** — implementare ora, dopo M13-02 SMP.
   Costo basso (campi TCB già presenti), beneficio alto (100% utilization teorica),
   certificabile. Default rimane FPP per retrocompatibilità.

2. **M16-NAS-01/02 Profiler** — implementare in parallelo a M13-01.
   Ring buffer lockless, overhead trascurabile. Utile anche senza il modello ML
   per debugging e WCET measurement (confluisce in M13-03 WCET Framework).

3. **M16-NAS-FPP** — implementare dopo il profiler.
   Porta valore immediato sui task soft-RT (blkd, vfsd, NSH) senza modificare
   le garanzie dei task FLAG_RT. Training offline su trace QEMU.

4. **M16-NAS-EDF** — implementare dopo M13-01 + M16-NAS-FPP.
   WCET adattivo elimina la stima manuale Ci, il costo critico di EDF.

### La tesi chiave

> EDF risolve il problema di **selezione ottimale** dato un insieme di parametri corretti.  
> NAS risolve il problema di **stima automatica** di quei parametri nel tempo.  
> Sono complementari, non alternativi.  
> Insieme portano a un sistema RT che è sia formalmente corretto che adattivo a carichi reali.

---

*Fine documento — aggiornare quando M13-01 e M16-NAS-01 saranno implementate.*
