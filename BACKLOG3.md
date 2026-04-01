# EnlilOS — Backlog 3: EnlilGFX 3D API e AI Framework
## Studio per rendering 3D Metal-like e sfruttamento ANE/GPU unificato

---

## Contesto e motivazione

Questo backlog parte dalla base già stabilita:

| Già implementato | Cosa offre |
|---|---|
| M3-03 ANE Syscall | Submit/wait inferenza ANE, pool buffer DMA, software fallback QEMU |
| M3-04 GPU Syscall | Command buffer, fence, GBO pool, compute dispatch, page-flip |
| M5b-01..04 | Driver virtio-gpu, display engine vsync, memory manager, 2D rendering |
| BACKLOG2 M11-04 | Mach-O compat (i binari macOS girerebbero già con Metal stub) |

**Obiettivo di Backlog 3:** costruire sopra questi mattoni un'API 3D di alto livello
(**EnlilGFX**) ispairata a Metal/DirectX 12 — esplicita, senza stato nascosto, con
Pipeline State Object, render pass e shader SPIR-V — e un framework AI di alto livello
(**EnlilML**) che espone ANE + GPU compute come un unico acceleratore unificato, con
capacità di *neural rendering* (upscaling, denoising, VRS saliency-driven).

---

## Architettura generale

```
┌────────────────────────────────────────────────────────────────┐
│                     Applicazione utente                        │
│                  (gioco, editor 3D, app ML)                    │
└──────────────────┬─────────────────────────┬───────────────────┘
                   │ EnlilGFX API             │ EnlilML API
       ┌───────────▼──────────┐   ┌───────────▼──────────────┐
       │  libegfx.a / .so     │   │  libenlilml.a / .so       │
       │  (M15–M18)           │   │  (M17–M19)                │
       └───────────┬──────────┘   └───────────┬──────────────┘
                   │                          │
       ┌───────────▼──────────────────────────▼──────────────┐
       │           EnlilGFX Backend Abstraction Layer         │
       │   virtio-gpu (QEMU)  │  Apple AGX (M-series)        │
       └───────────┬──────────────────────────┬──────────────┘
                   │                          │
       ┌───────────▼──────────┐   ┌───────────▼──────────────┐
       │  M3-04 GPU Syscall   │   │  M3-03 ANE Syscall        │
       │  (command buf, fence)│   │  (inference submit/wait)  │
       └──────────────────────┘   └──────────────────────────┘
```

---

## Principi di design EnlilGFX

| Principio | Regola |
|---|---|
| **Esplicito su implicito** | Nessuno stato globale. Ogni oggetto porta il suo stato. Ispirato a Metal/DX12, non a OpenGL |
| **PSO immutabili** | Pipeline State Object compilato una volta al setup — nessuna ricompilazione nel frame loop |
| **Render pass dichiarativi** | Attachments dichiarati prima di encodare i draw call — permette al tile renderer AGX di ottimizzare |
| **Fence espliciti** | Nessuna sincronizzazione implicita — il programmatore gestisce CPU/GPU sync con fence |
| **Backend-agnostico** | L'app scrive contro `egfx_*` — il backend compila su virtio-gpu (QEMU) o AGX (M-series) |
| **SPIR-V come IR** | Linguaggio shader → SPIR-V → backend specifico. Zero dipendenza da MSL o HLSL nel core |
| **Zero alloc nel frame loop** | Tutti gli oggetti EnlilGFX vengono creati al setup e riusati ogni frame |

---

## MILESTONE 15 — EnlilGFX: Oggetti Core

### ⬜ M15-01 · Device, Queue e Heap

Il punto di ingresso dell'API. Mappa 1:1 sulle astrazione Metal/DX12.

**Device** (`egfx_device_t`): rappresenta la GPU fisica. Una sola istanza per processo.
- `egfx_device_create(egfx_device_desc_t *, egfx_device_t **out)`
  - Interroga `gpu_query_caps()` (M3-04 nr 120), seleziona backend (AGX o virtio-gpu)
  - Alloca i pool interni: PSO, heap, fence, semafori (tutti statici, dimensione max configurabile)
- `egfx_device_destroy(egfx_device_t *)`

**Command Queue** (`egfx_queue_t`): corrisponde a `gpu_cmdqueue_create()` (nr 125).
- Tipi: `EGFX_QUEUE_RENDER`, `EGFX_QUEUE_COMPUTE`, `EGFX_QUEUE_BLIT`
- Più code per parallelismo CPU/GPU (es. render su queue 0, shadow map su queue 1)
- `egfx_queue_submit(queue, cmdbuf, signal_fence, wait_fence)`
  - Mappa su `gpu_cmdbuf_submit()` (nr 128)

**Heap** (`egfx_heap_t`): regione di memoria GPU gestita esplicitamente.
- `EGFX_HEAP_PRIVATE` — solo GPU accessibile (ideale per texture, depth buffer)
- `EGFX_HEAP_SHARED`  — CPU + GPU accessibile (uniform buffer, staging)
- `EGFX_HEAP_MEMLESS` — transient: esiste solo durante un render pass (tile memory AGX)
- Implementazione: wrapper attorno ai pool GBO del GPU memory manager (M5b-03)
- Sub-allocazione: bump allocator dentro l'heap per risorse piccole — O(1), no fragmentation

```c
typedef struct {
    uint32_t         type;            /* EGFX_HEAP_* */
    size_t           size;            /* byte totali */
    uint32_t         flags;           /* EGFX_HEAP_CPU_CACHED | EGFX_HEAP_COHERENT */
} egfx_heap_desc_t;

typedef struct {
    gpu_buf_handle_t gbo;             /* GBO sottostante (M3-04) */
    void            *cpu_ptr;         /* non-NULL se HEAP_SHARED */
    size_t           size;
    size_t           offset_cursor;   /* bump allocator */
} egfx_heap_t;
```

---

### ⬜ M15-02 · Shader Library e Pipeline State Object (PSO)

Il cuore dell'API. Nessun draw call può avvenire senza un PSO.

#### Shader Library (`egfx_library_t`)

Contenitore di shader compilati. Input: binario SPIR-V (o, in futuro, EGSL compilato).

- `egfx_library_create(device, spv_bytes, spv_size) → egfx_library_t *`
  - Su AGX: invoca il compilatore offline AGX per produrre il binario nativo (vedi M16-03)
  - Su virtio-gpu: carica il SPIR-V nel `GPU_BUF_SHADER` tramite virgl (vedi M16-04)
  - Caching: hash SHA-256 del SPIR-V → cerca in cache disco (`/var/cache/egfx/`) prima di ricompilare
- `egfx_library_get_function(lib, name) → egfx_function_t *` — recupera vertex/fragment/compute function per nome

#### Render Pipeline State Object (`egfx_render_pso_t`)

Compila staticamente tutto lo stato fisso della pipeline 3D:

```c
typedef struct {
    /* Shader */
    egfx_function_t  *vertex_function;
    egfx_function_t  *fragment_function;

    /* Vertex input layout */
    egfx_vertex_attr_t  attributes[16];  /* binding, offset, format, stride */
    uint32_t            n_attributes;

    /* Pixel format attachments */
    egfx_pixel_format_t color_formats[8];
    egfx_pixel_format_t depth_format;
    egfx_pixel_format_t stencil_format;

    /* Blend state per attachment */
    egfx_blend_desc_t   blend[8];

    /* Rasterizer */
    egfx_cull_mode_t    cull_mode;       /* NONE / FRONT / BACK */
    egfx_fill_mode_t    fill_mode;       /* FILL / WIREFRAME */
    float               depth_bias;
    int                 depth_clip;

    /* Depth/stencil */
    egfx_depth_stencil_desc_t depth_stencil;

    /* Sample count (MSAA) */
    uint32_t            sample_count;    /* 1, 2, 4, 8 */
} egfx_render_pso_desc_t;
```

- `egfx_render_pso_create(device, desc) → egfx_render_pso_t *`
  - Compilazione: PSO è **immutabile** dopo la creazione — nessuna ricompilazione nel frame loop
  - Costo: ~ 1-50ms (solo al setup, mai durante rendering)
- `egfx_render_pso_destroy(pso)`

#### Compute Pipeline State Object (`egfx_compute_pso_t`)

Versione semplificata per compute shader (nessuno stato di rasterizzazione):

```c
typedef struct {
    egfx_function_t  *compute_function;
    uint32_t          threadgroup_size[3];  /* x, y, z */
} egfx_compute_pso_desc_t;
```

---

### ⬜ M15-03 · Render Pass e Framebuffer

Dichiarazione esplicita degli attachment prima di encodare i draw call.
Su AGX (TBDR) questa informazione è **critica** per il tile memory layout.

```c
typedef struct {
    /* Attachment colore (max 8) */
    struct {
        egfx_texture_t      *texture;
        uint32_t             level;
        uint32_t             slice;
        egfx_load_action_t   load;    /* LOAD / CLEAR / DONT_CARE */
        egfx_store_action_t  store;   /* STORE / DISCARD / RESOLVE (MSAA) */
        egfx_color_t         clear;   /* usato se load = CLEAR */
    } color[8];
    uint32_t n_color_attachments;

    /* Attachment depth */
    struct {
        egfx_texture_t      *texture;
        egfx_load_action_t   load;
        egfx_store_action_t  store;
        float                clear_depth;
    } depth;

    /* Attachment stencil */
    struct {
        egfx_texture_t      *texture;
        egfx_load_action_t   load;
        egfx_store_action_t  store;
        uint32_t             clear_stencil;
    } stencil;
} egfx_render_pass_desc_t;
```

- `egfx_render_pass_begin(cmdbuf, desc) → egfx_render_encoder_t *`
  - Su AGX: emette il comando `SetRenderTargets` nel ring AGX con tile memory layout calcolato
  - Su virtio-gpu: emette `VCMD_SET_FRAMEBUFFER` via virgl
- `egfx_render_pass_end(encoder)` — chiude il pass, flush tile memory → main memory (AGX)

**Subpass** (ottimizzazione AGX): render passes annidati che condividono tile memory.
Esempio classico: G-Buffer pass + lighting pass senza roundtrip main memory.

---

### ⬜ M15-04 · Texture e Sampler State

#### Texture (`egfx_texture_t`)

```c
typedef struct {
    egfx_texture_type_t  type;    /* 2D / 3D / CUBE / 2D_ARRAY / 2D_MS */
    egfx_pixel_format_t  format;  /* RGBA8_UNORM, BGRA8_UNORM, DEPTH32F, BC1-7, ASTC, ... */
    uint32_t             width;
    uint32_t             height;
    uint32_t             depth;   /* 1 per 2D */
    uint32_t             mip_levels;
    uint32_t             array_length;
    uint32_t             sample_count;
    egfx_texture_usage_t usage;   /* SHADER_READ | RENDER_TARGET | SHADER_WRITE */
    egfx_heap_t         *heap;    /* se NULL: alloca da heap privato interno */
} egfx_texture_desc_t;
```

- `egfx_texture_create(device, desc) → egfx_texture_t *`
- `egfx_texture_replace_region(tex, region, mip, slice, data, bytes_per_row)` — upload CPU→GPU
  - Internamente usa un **staging buffer** su heap SHARED + blit command (M15-05 blit encoder)
- Formati compressi supportati:
  - QEMU: `BC1-BC7` (virtio-gpu DX11 feature level)
  - AGX: `ASTC` (4x4 a 12x12), `BC1-BC7` (M3+)

#### Sampler State (`egfx_sampler_t`)

Oggetto immutabile. Compilato in hardware sampler descriptor:

```c
typedef struct {
    egfx_min_filter_t  min_filter;   /* NEAREST / LINEAR */
    egfx_mag_filter_t  mag_filter;
    egfx_mip_filter_t  mip_filter;   /* NONE / NEAREST / LINEAR */
    egfx_address_mode_t s, t, r;     /* REPEAT / CLAMP / MIRROR / BORDER */
    float              lod_min, lod_max;
    float              lod_bias;
    uint32_t           max_anisotropy;  /* 1, 2, 4, 8, 16 */
    egfx_compare_func_t compare;     /* per shadow samplers */
} egfx_sampler_desc_t;
```

---

### ⬜ M15-05 · Buffer (Vertex, Index, Uniform, Storage)

Tutti i tipi di buffer si creano come `egfx_buffer_t` — si distinguono per l'uso:

```c
typedef struct {
    size_t        size;
    egfx_heap_t  *heap;    /* NULL = heap privato interno */
    uint32_t      usage;   /* EGFX_BUF_VERTEX | INDEX | UNIFORM | STORAGE | INDIRECT */
} egfx_buffer_desc_t;

typedef struct {
    gpu_buf_handle_t gbo;       /* GBO sottostante (M3-04) */
    void            *cpu_ptr;   /* non-NULL se su heap SHARED */
    size_t           size;
    uint32_t         usage;
} egfx_buffer_t;
```

- `egfx_buffer_create(device, desc) → egfx_buffer_t *`
- `egfx_buffer_contents(buf) → void *` — solo se SHARED; ritorna `cpu_ptr`
- `egfx_buffer_did_modify_range(buf, offset, size)` — invalida cache CPU, segnala GPU

**Uniform buffer binding** — tre stili supportati:

1. **Push constants** (≤ 64 byte): trasmessi inline nel command buffer — latenza zero
2. **Descriptor** (> 64 byte): binding esplicito a slot `[[buffer(N)]]`
3. **Argument buffer** (Metal-style): struttura di descriptor GPU-resident per bindless rendering

---

### ⬜ M15-06 · Command Encoder (Render, Compute, Blit)

Oggetto che registra i comandi in un command buffer GPU (M3-04 `gpu_cmdbuf_t`).

#### Render Encoder

```c
/* Configurazione pipeline */
void egfx_enc_set_render_pso  (enc, pso);
void egfx_enc_set_vertex_buf  (enc, buf, offset, index);
void egfx_enc_set_index_buf   (enc, buf, offset, format);  /* UINT16 / UINT32 */
void egfx_enc_set_texture      (enc, tex, stage, slot);    /* stage: VS/FS */
void egfx_enc_set_sampler      (enc, smp, stage, slot);
void egfx_enc_set_uniform_buf  (enc, buf, offset, size, stage, slot);
void egfx_enc_set_push_const   (enc, data, size, stage, slot);  /* ≤ 64B inline */

/* Viewport e scissor */
void egfx_enc_set_viewport     (enc, x, y, w, h, znear, zfar);
void egfx_enc_set_scissor_rect (enc, x, y, w, h);

/* Draw call */
void egfx_enc_draw             (enc, prim, vertex_start, vertex_count);
void egfx_enc_draw_indexed     (enc, prim, index_count, index_offset, vertex_offset);
void egfx_enc_draw_instanced   (enc, prim, vertex_count, instance_count, base_instance);
void egfx_enc_draw_indirect    (enc, prim, buf, buf_offset);  /* GPU-driven (M18-02) */
```

#### Compute Encoder

```c
void egfx_enc_set_compute_pso     (enc, pso);
void egfx_enc_set_buf             (enc, buf, offset, slot);
void egfx_enc_set_tex_read        (enc, tex, slot);
void egfx_enc_set_tex_write       (enc, tex, slot);
void egfx_enc_dispatch_threadgroups (enc, gx, gy, gz);
void egfx_enc_dispatch_indirect   (enc, buf, offset);        /* GPU-driven */
```

#### Blit Encoder

```c
void egfx_enc_copy_buf_to_tex  (enc, src_buf, src_off, bytes_per_row,
                                     dst_tex, dst_mip, dst_slice, dst_origin);
void egfx_enc_copy_tex_to_buf  (enc, src_tex, src_mip, src_slice, src_origin,
                                     dst_buf, dst_off, bytes_per_row);
void egfx_enc_copy_tex_to_tex  (enc, src, dst, region);
void egfx_enc_fill_buf         (enc, buf, range, value);
void egfx_enc_generate_mipmaps (enc, tex);
```

---

### ⬜ M15-07 · Software Rasterizer Fallback

Per sviluppo su hardware senza GPU (es. QEMU senza `-device virtio-gpu-pci`):

- **TinyGL-like** (~2000 righe C): rasterizzazione scanline, Z-buffer, vertex transform
- `EGFX_BACKEND=SW` env var o rilevamento automatico se `gpu_query_caps()` ritorna `GPU_CAP_SWFALLBACK`
- Non performante (max ~5fps a 1080p) ma corretto — utile per CI/CD e test headless
- Output: scrive direttamente nel framebuffer UART/MMIO (pre-M5b)
- Vertex shader simulato in C: trasformazione MVP via NEON AArch64 (`float32x4_t`)
- Fragment shader: funzione C per ogni PSO — generata al momento della `egfx_render_pso_create()`
  tramite composizione di funzioni (non JIT; nessuna esecuzione di codice generato a runtime)

---

## MILESTONE 16 — Shader Compilation Pipeline

> **Principio:** lo shader viene scritto una volta in EGSL o GLSL/HLSL,
> compilato offline in SPIR-V, e il backend EnlilGFX lo traduce nella ISA
> specifica (AGX o virgl) al momento della `egfx_library_create()`.
> Il frame loop non compila mai shader — nessuna varianza WCET da compilazione.

---

### ⬜ M16-01 · EGSL — Enlil Graphics Shading Language

Sottoinsieme di **GLSL 4.60** con estensioni EnlilGFX:

- Annotazioni Metal-style: `[[vertex]]`, `[[fragment]]`, `[[compute]]`, `[[buffer(N)]]`,
  `[[texture(N)]]`, `[[sampler(N)]]`, `[[position]]`, `[[stage_in]]`
- Tipi GLSL standard: `vec2/3/4`, `mat3/4`, `sampler2D`, `image2D`, `layout(binding=N)`
- Estensioni: `@push_constant` per uniform ≤ 64B inline; `@threadgroup` per memoria condivisa compute
- Compilatore offline `egslc`:
  - Front-end: parser EGSL → AST
  - Middle-end: validazione tipo, inlining, dead code elimination
  - Back-end: emissione SPIR-V 1.5 (Khronos standard)
  - Integrazione Makefile: `$(EGSLC) shader.egsl -o shader.spv`
- Dipende solo da: `spirv-tools` (validazione) — nessuna dipendenza da LLVM o glslang

---

### ⬜ M16-02 · SPIR-V Frontend

Ingestion diretta di SPIR-V precompilato:

- Parser SPIR-V 1.5: legge header magic `0x07230203`, words, opcode
- Validazione strutturale: entry point, tipi, decorazioni `Binding`/`Location`/`BuiltIn`
- Reflection: estrae automaticamente l'interfaccia shader (vertex inputs, uniform bindings,
  push constants) per popolare automaticamente i descriptor del PSO
- `spirv-opt` integration (offline, non a runtime): eliminazione dead code, constant folding,
  loop unrolling — migliora la qualità del codice emesso dal backend
- Il SPIR-V validato viene memorizzato nel `egfx_library_t` come buffer opaco

---

### ⬜ M16-03 · Backend AGX (SPIR-V → AGX ISA)

Compilazione da SPIR-V alla ISA proprietaria Apple AGX. Due approcci:

**Approccio A (consigliato): `mesa3d/NVK`-style compiler**
- Usa il compiler `libagx` di Mesa (Asahi) come libreria offline
- Input: SPIR-V; Output: binario AGX (`.bin`) + metadata (register usage, tile memory)
- Invocato a compile-time come tool host (`tools/agx_compile`), output embedded nel binario
- A runtime: `egfx_library_create()` carica il binario precompilato dal filesystem

**Approccio B (fallback): cross-compile SPIR-V → MSL → offline xcrun**
- `spirv-cross` converte SPIR-V → MSL (Metal Shading Language)
- `xcrun -sdk macosx metal -arch air64 shader.metal` → `.metallib`
- Solo quando si fa toolchain host macOS; non serve a runtime su EnlilOS

**Struttura del binario shader AGX:**
```c
typedef struct {
    uint32_t  magic;          /* 0xAGX00001 */
    uint32_t  stage;          /* VERTEX / FRAGMENT / COMPUTE */
    uint32_t  code_offset;
    uint32_t  code_size;
    uint32_t  n_uniforms;
    uint32_t  n_textures;
    uint32_t  n_samplers;
    uint32_t  threadgroup_mem; /* bytes tile memory richiesti */
    uint8_t   code[];          /* ISA AGX */
} agx_shader_binary_t;
```

---

### ⬜ M16-04 · Backend virgl (SPIR-V → virgl per QEMU)

virgl (Virtual GL) è il renderer 3D di QEMU `virtio-gpu`:

- QEMU con `-device virtio-gpu-gl` espone OpenGL ES 3.1 + GLES extensions
- virgl accetta SPIR-V via estensione `GL_ARB_gl_spirv` (OpenGL 4.6) o
  `GL_EXT_spirv_intrinsics` (GLES 3.2)
- `egfx_library_create()` su backend virgl: invia il SPIR-V al host QEMU via
  `VCMD_BIND_SHADER` della virgl command stream
- Vertex/fragment/compute shader caricati direttamente senza ricompilazione host-side
- Uniform buffer: `VCMD_SET_UNIFORM_BUFFER`
- Texture: `VCMD_RESOURCE_CREATE` + `VCMD_TRANSFER_TO_HOST`
- Dipende da: `QEMU >= 8.0` con `-device virtio-gpu-gl` e host con driver OpenGL 4.6

---

### ⬜ M16-05 · Shader Cache e Hot Reload

- Cache su disco: `sha256(spv) → /var/cache/egfx/<hash>.agx` o `.virgl`
- `egfx_library_create()` controlla la cache prima di ricompilare — amortizza il costo
- Invalidazione cache: se il binario AGX è più vecchio del SPIR-V sorgente → ricompila
- **Hot reload** (solo modalità debug): `inotify`-like su `/dev/fsevents` (futuro) o
  polling periodico su mtime shader file; se cambiato → ricompila + sostituisce `egfx_library_t`
  senza fermare il processo (pipeline in volo completano con il PSO precedente via fence)

---

## MILESTONE 17 — EnlilML: Framework AI di Alto Livello

> **Principio:** EnlilML astrae ANE + GPU compute in un unico acceleratore logico.
> Il framework sceglie autonomamente dove eseguire ogni layer: ANE per reti neurali
> standard, GPU compute per operazioni custom o quando ANE è occupato.
> Su QEMU tutto gira su CPU via software fallback trasparente.

---

### ⬜ M17-01 · ML Daemon (`mld`) — Model Server

Processo user-space a priorità media, wrappa le syscall ANE di M3-03.

- `mld` si avvia al boot, espone una **porta IPC** per richieste inference
- **Motivo:** centralizza la gestione dei modelli caricati, evita che ogni processo
  carichi lo stesso modello N volte nella SRAM ANE
- **API IPC** (messaggi < 64 byte, zero-copy via capability su buffer DMA condiviso):

```c
/* Richiesta caricamento modello */
typedef struct { uint32_t cmd; cap_t model_buf_cap; uint32_t size; } mld_load_req_t;
/* Risposta: model_handle */
typedef struct { uint32_t cmd; uint32_t model_handle; int32_t err; } mld_load_rsp_t;

/* Richiesta inferenza */
typedef struct {
    uint32_t  cmd;
    uint32_t  model_handle;
    cap_t     input_cap;        /* capability buffer DMA input */
    cap_t     output_cap;       /* capability buffer DMA output */
    uint64_t  deadline_ns;      /* 0 = best effort */
    uint32_t  priority;         /* eredita dal chiamante */
} mld_infer_req_t;
/* Risposta: job_handle per polling/wait */
typedef struct { uint32_t cmd; uint32_t job_handle; int32_t err; } mld_infer_rsp_t;
```

- `mld` gestisce una **coda di priorità** dei job (max 32 slot statici)
- Priority inheritance: se il chiamante è a priorità alta → `mld` esegue
  temporaneamente alla stessa priorità (sched_set_priority IPC)
- Scheduling dei job ANE: EDF (Earliest Deadline First) sui job con deadline esplicita

---

### ⬜ M17-02 · Tensor Abstraction Layer

Struttura dati unificata per tensori usata da EnlilML e da EnlilGFX (neural rendering):

```c
/* Layout di memoria supportati */
typedef enum {
    ENLILML_NHWC  = 0,   /* batch, height, width, channel — standard TFLite */
    ENLILML_NCHW  = 1,   /* batch, channel, height, width — standard PyTorch */
    ENLILML_HWCX  = 2,   /* formato tiled Apple ANE (tile 64B packed) */
} enlilml_layout_t;

/* Tipo elemento */
typedef enum {
    ENLILML_FLOAT32 = 0,
    ENLILML_FLOAT16 = 1,
    ENLILML_INT8    = 2,   /* quantizzato */
    ENLILML_UINT8   = 3,   /* quantizzato unsigned */
    ENLILML_BFLOAT16= 4,
} enlilml_dtype_t;

typedef struct {
    uint32_t        shape[5];   /* max 5 dimensioni */
    uint32_t        ndim;
    enlilml_layout_t layout;
    enlilml_dtype_t  dtype;
    cap_t            buf_cap;   /* capability sul buffer DMA */
    void            *cpu_ptr;   /* non-NULL se mappato in CPU address space */
    size_t           byte_size;
} enlilml_tensor_t;
```

**Operazioni CPU-side sul tensore** (NEON AArch64, non su ANE):
- `enlilml_tensor_convert_layout(src, dst_layout)` — NHWC ↔ HWCX
- `enlilml_tensor_quantize_int8(src_f32, scale, zero_point) → dst_int8`
- `enlilml_tensor_dequantize_int8(src_int8, scale, zero_point) → dst_f32`
- Tutte con WCET O(N) e NEON intrinsics per throughput × 4

---

### ⬜ M17-03 · ONNX Model Loader

ONNX (Open Neural Network Exchange) è il formato interchange standard per reti neurali.

- Parser protobuf minimale: solo i tipi usati in ONNX (nessuna dipendenza da libprotobuf)
  - `VarInt` + `LengthDelimited` encoding, ~500 righe C
- Layer ONNX supportati (subset per inferenza comune):
  `Conv`, `BatchNormalization`, `ReLU`, `MaxPool`, `AveragePool`,
  `Gemm` (fully connected), `Reshape`, `Transpose`, `Concat`,
  `Add`, `Mul`, `Sigmoid`, `Softmax`, `Upsample`/`Resize`
- Compilazione ONNX → `.hwx` (formato ANE): via tool offline `tools/onnx_to_hwx`
  basato su `libANECompiler` (Asahi Linux reverse engineering)
- Su QEMU: ONNX → sequenza di chiamate CPU (software fallback `ane_sw.c`)
- `enlilml_model_load(path) → enlilml_model_t *`
  - Carica da VFS, compila se necessario, invia a `mld` per caching

---

### ⬜ M17-04 · Pipeline di Inferenza Unificata ANE + GPU

Gestione automatica del backend più efficiente per ogni layer:

**Strategia di dispatch:**
```
Per ogni layer nel modello:
  se layer è supportato dall'ANE hardware E ANE non è al 100%:
      → submit su ANE (M3-03 ane_inference_submit)
  altrimenti se layer è implementato come compute shader:
      → dispatch su GPU (M3-04 gpu_compute_dispatch)
  altrimenti:
      → esecuzione CPU fallback
```

- **Fusione di layer** (layer fusion): Conv + BatchNorm + ReLU vengono fusi in un
  singolo job ANE — riduce trasferimenti dati, aumenta throughput 3-4×
- **Double buffering inferenza**: mentre ANE elabora il batch N, la CPU prepara il batch N+1
  — zero idle time sull'acceleratore
- **Timeout adattivo**: se l'inferenza supera `deadline_ns * 0.9` → job successivo viene
  schedulato prima per rispettare la deadline del chiamante
- API pubblica:

```c
/* Esegue inferenza. Non-blocking: ritorna job_handle. */
enlilml_job_t enlilml_infer_async(model, input_tensor, output_tensor, deadline_ns);

/* Attende completamento con timeout. */
int enlilml_infer_wait(job, timeout_ns);

/* Sincrono (= async + wait senza timeout) — mai da task hard-RT */
int enlilml_infer_sync(model, input, output);

/* Query stato senza bloccare */
enlilml_job_status_t enlilml_job_query(job);
```

---

### ⬜ M17-05 · Neural Bridge GPU ↔ ANE (Zero-Copy)

**Obiettivo:** un compute shader GPU può alimentare direttamente l'ANE senza roundtrip
CPU, e viceversa il risultato ANE può alimentare uno shader GPU senza copia.

- **GBO condiviso GPU-ANE**: su M-series, GPU e ANE condividono la stessa DRAM fisica.
  Un `gpu_buf_handle_t` può essere passato direttamente come `ane_buf_handle_t` se
  l'allocazione avviene sulla regione condivisa (`EGFX_HEAP_ANE_SHARED`)
- Procedura:
  1. Allocare tensor su heap `ENLILML_BUF_GPU_ANE` (nuovo flag M5b-03)
  2. GPU scrive dati via compute dispatch → fence GPU → segnale ANE
  3. ANE legge dati direttamente dalla stessa regione → inferenza
  4. ANE scrive output → segnale GPU → GPU legge risultato come texture
- Latenza roundtrip GPU→ANE→GPU: ~ 100–300µs (vs ~2ms con copia CPU intermedia)
- Su QEMU: GBO e buffer ANE sono entrambi in RAM → zero-copy già garantito per struttura

---

## MILESTONE 18 — Funzionalità GPU Avanzate

### ⬜ M18-01 · Hardware Ray Tracing (AGX M3/M4+)

Apple AGX M3 e M4 integrano unità hardware per BVH traversal (Ray Tracing accelerato).

- **Acceleration Structure** (`egfx_accel_struct_t`):
  - Bottom-Level AS (BLAS): geometria del singolo mesh (vertex + index buffer)
  - Top-Level AS (TLAS): istanze di BLAS con transform matrix
  - `egfx_blas_create(device, geo_desc) → egfx_accel_struct_t *`
  - `egfx_tlas_create(device, instances, count) → egfx_accel_struct_t *`
  - Build asincrono via compute encoder: `egfx_enc_build_accel_struct(enc, as, scratch_buf)`

- **Ray generation shader**: compute shader che genera i raggi da dispatching
- **Intersection shader**: sostituisce la geometria con forme custom (es. sfere, SDF)
- **Any-hit / Closest-hit shader**: eseguiti quando un raggio interseca la geometria
- **Miss shader**: eseguito quando nessuna intersezione trovata → campiona env map

**Ray tracing command encoder:**
```c
void egfx_enc_set_rt_pso         (enc, rt_pso);
void egfx_enc_set_accel_struct   (enc, tlas, slot);
void egfx_enc_dispatch_rays      (enc, ray_gen_buf, w, h, depth);
```

- Supporto hardware: AGX M3 / M4. Su M1 / M2 e QEMU → software fallback BVH CPU
  (DFS iterativo, ~1fps a 1080p — solo per correttezza, non performance)
- Caso d'uso primario su EnlilOS: **ambient occlusion RT** e **shadow ray** per
  il compositor grafico (M12-03), con budget = 2ms per frame a 60Hz

---

### ⬜ M18-02 · Indirect Command Buffer (GPU-Driven Rendering)

Elimina il collo di bottiglia CPU nel loop di draw: la GPU genera i propri draw call.

- `egfx_indirect_cmdbuf_t`: buffer GPU che contiene comandi pre-scritti dalla CPU
  o generati da un compute shader
- `egfx_enc_execute_indirect_cmdbufs(render_enc, icb, range)` — GPU esegue i comandi
  nel buffer senza coinvolgere la CPU
- **GPU culling**: compute shader valuta frustum/occlusion per ogni mesh → scrive solo
  i draw call delle mesh visibili nell'ICB → la CPU non vede mai la lista filtrata
- **Benefit RT**: il frame loop diventa un singolo `submit` + `present` — WCET costante
  indipendente dal numero di oggetti nella scena
- Richiede AGX M1+ (Metal 3 feature). Su QEMU: emulazione CPU (il kernel esegue i comandi
  dal buffer uno per uno — corretto ma non GPU-driven)

---

### ⬜ M18-03 · Mesh Shader (AGX M4)

Sostituisce la pipeline vertex/geometry shader con una pipeline programmabile a due stadi:

- **Object shader** (amplification): determina quanti mesh shader lanciare per ogni
  unità di lavoro (es. LOD selection, frustum culling per tile)
- **Mesh shader**: genera primitive (triangoli) direttamente, senza vertex buffer fisso
  — ideale per generazione procedurale, impostor, terrain clipmap
- `egfx_mesh_pso_t`: pipeline con `object_function` + `mesh_function` + `fragment_function`
- `egfx_enc_draw_mesh_threadgroups(enc, ox, oy, oz)`
- Richiede AGX M4. Su versioni precedenti → fallback sulla pipeline vertex standard

---

### ⬜ M18-04 · Variable Rate Shading (VRS)

Riduce la frequenza di campionamento del fragment shader nelle regioni a bassa importanza
visiva — risparmio GPU del 20–40% su scene complesse.

- **Tier 1 — Rate per draw call**: `egfx_enc_set_shading_rate(enc, rate)` dove
  `rate ∈ { 1×1, 1×2, 2×1, 2×2, 4×4 }`
- **Tier 2 — Rate per tile via shading rate image**: texture 8×8 tile dove ogni texel
  indica il rate per quel tile dello schermo → caricata come `[[shading_rate_image]]`
- **Tier 3 — Rate combinato**: max(draw call rate, tile rate) — hardware decide
- Su QEMU: VRS ignorato (tutti i fragment shaded a 1×1) — nessun impatto sulla correttezza

---

## MILESTONE 19 — Neural Rendering

> **Principio:** l'ANE non è solo un acceleratore per l'IA — è un renderer ausiliario.
> EnlilOS integra ANE e GPU nella stessa pipeline di rendering per ottenere qualità
> e performance che né GPU né ANE da soli potrebbero raggiungere.

---

### ⬜ M19-01 · Neural Upscaling (ANLSS — Enlil Neural Super Sampling)

Analogo a DLSS (NVIDIA) o MetalFX Upscaling (Apple), usando l'ANE come upscaler.

**Pipeline:**
1. Rendi la scena a **risoluzione ridotta** (es. 960×540) con EnlilGFX → 4ms per frame
2. ANE esegue il modello di upscaling (rete convoluzionale 7-layer) → 2ms per frame
3. Output: frame a risoluzione nativa (1920×1080) → `gpu_present()` → display

**Modello ANE di upscaling:**
- Architettura: ESRGAN-tiny modificata (15MB, int8 quantizzata → 3.8MB su disco)
- Input: frame a bassa risoluzione (RGBA8) + motion vectors (RG16F)
- Output: frame upscalato (RGBA8)
- Training offline (non su EnlilOS): PyTorch + ONNX export → `onnx_to_hwx` → `.hwx`
- TOPS richiesti: ~1.5 TOPS (disponibili già su M1 con 11 TOPS totali)

**Integrazione con EnlilGFX:**
- `egfx_neural_upscale_init(device, model_path, src_w, src_h, dst_w, dst_h) → egfx_upscaler_t *`
- `egfx_neural_upscale_frame(upscaler, src_tex, motion_vec_tex, dst_tex, deadline_ns) → enlilml_job_t`
- Zero-copy via `ENLILML_BUF_GPU_ANE` (M17-05): la texture GPU diventa input ANE direttamente

**Fallback su QEMU:** bicubic upscaling via compute shader — non AI, ma stessa interfaccia API.

---

### ⬜ M19-02 · AI Denoising per Path Tracing

**Contesto:** il path tracer RT (M18-01) produce immagini rumorose a basso spp (samples per pixel).
Un denoiser AI produce immagini pulite da 1-4spp, equivalenti visivamente a 256spp.

**Pipeline:**
1. Path tracing a 1spp via ray tracing hardware (M18-01) → 3ms per frame
2. G-Buffer auxiliary inputs: albedo, normal, depth (da G-Buffer render pass)
3. ANE denoiser: OIDN-tiny (Intel Open Image Denoise portato su ANE) → 4ms per frame
4. Output: frame denoised → compositor (M12-03)

**Modello ANE:**
- Basato su OIDN 2.x: U-Net con ~4M parametri, int8 quantizzato → ~4MB
- Input: noisy color (RGBA16F) + albedo (RGB8) + normal (RGB16F)
- Output: denoised color (RGBA16F)
- TOPS: ~2.5 TOPS — rientra nel budget ANE anche su M1

**API:**
- `egfx_denoiser_create(device, model_path) → egfx_denoiser_t *`
- `egfx_denoiser_run(denoiser, noisy_tex, albedo_tex, normal_tex, output_tex, deadline_ns)`

---

### ⬜ M19-03 · Saliency-Based Variable Rate Shading

Combina M18-04 (VRS) con l'ANE per ridurre automaticamente il rate nelle zone
periferiche o a bassa attenzione visiva, massimizzando il risparmio GPU.

**Pipeline:**
1. Frame precedente → ANE esegue modello di saliency detection (eye tracking proxy)
   → produce **saliency map** 8×8 tile in 0.5ms
2. Compute shader converte saliency map → VRS rate image (Tier 2)
3. Frame corrente renderizzato con VRS: centro = 1×1, periferia = 4×4
4. Risparmio atteso: 30–50% GPU compute su scene complesse

**Modello ANE saliency:**
- Rete leggera (MobileNetV3-tiny adattata): input = 128×128 frame scaled, output = 16×16 heatmap
- ~0.3 TOPS, < 0.3ms — trascurabile sul budget ANE

**Integrazione:**
- `egfx_vrs_saliency_init(device, model_path) → egfx_vrs_saliency_t *`
- Chiamata una volta per frame prima del render pass principale
- La VRS rate image risultante viene passata al render encoder tramite
  `egfx_enc_set_shading_rate_image(enc, vrs_tex)`

---

### ⬜ M19-04 · Neural Material Synthesis

Generazione procedurale di texture tramite ANE — elimina la necessità di albedo map
ad alta risoluzione pre-baked.

**Applicazione:** terrain rendering, materiali dinamici (ruggine, usura, sporco)
che cambiano in base allo stato del gioco senza texture statiche.

**Pipeline:**
- Input: parametri materiale (tipo, usura, umidità — vettore 16 float)
- ANE: rete generativa (decoder di un VAE) → albedo + normal + roughness map 256×256
- Output: 3 texture → bind come `[[texture(0/1/2)]]` nel fragment shader
- Aggiornamento: solo quando i parametri cambiano, non ogni frame

**Modello ANE:**
- Architettura: decoder VAE ~1M parametri, int8 → 1MB
- Latenza: ~0.8ms per texture 256×256 su M1 ANE

---

## Struttura directory (nuovo codice di Backlog 3)

```
lib/egfx/
    egfx.h                   — API pubblica EnlilGFX
    egfx_device.c            — device, queue, heap
    egfx_pso.c               — PSO render e compute
    egfx_render_pass.c       — render pass encoder
    egfx_texture.c           — texture e sampler
    egfx_buffer.c            — buffer management
    egfx_encoder.c           — command encoder (render/compute/blit)
    egfx_sw_rast.c           — software rasterizer fallback
    backends/
        agx_backend.c        — backend AGX (M-series)
        virgl_backend.c      — backend virtio-gpu virgl (QEMU)
        sw_backend.c         — backend CPU puro

lib/enlilml/
    enlilml.h                — API pubblica EnlilML
    tensor.c                 — tensor abstraction + conversioni NEON
    onnx_loader.c            — parser ONNX + dispatch ANE/GPU/CPU
    neural_bridge.c          — zero-copy GPU↔ANE
    upscaler.c               — neural upscaling (M19-01)
    denoiser.c               — AI denoising (M19-02)
    vrs_saliency.c           — saliency-based VRS (M19-03)
    material_synth.c         — neural material synthesis (M19-04)

servers/mld/
    main.c                   — ML daemon: IPC server, model cache, job queue

tools/
    egslc/                   — EGSL compiler (GLSL subset → SPIR-V)
    agx_compile/             — SPIR-V → AGX ISA (offline, host tool)
    onnx_to_hwx/             — ONNX → ANE .hwx format (offline, host tool)

models/
    upscaler_int8.onnx       — ESRGAN-tiny per ANLSS
    denoiser_int8.onnx       — OIDN-tiny per path tracing
    saliency_int8.onnx       — MobileNetV3-tiny per VRS saliency
```

---

## Dipendenze Backlog 3

```
/* Base kernel (Backlog 1) */
M3-03 (ANE syscall) ──────────────────────────────────┐
M3-04 (GPU syscall) ──────────────────────────────────┤
M5b-01..04 (GPU driver, display, 2D) ─────────────────┤
                                                       │
/* Backlog 2 */                                        │
M11-01 (musl libc) ───────────────────────────────────┤
M13-02 (SMP, compilazione parallela) ─────────────────┤
M9-02 (vfsd, per shader cache su disco) ──────────────┤
                                                       ▼
M15-01 (device, queue, heap)
M15-01 → M15-02 (shader library + PSO)
M15-02 → M15-03 (render pass)
M15-03 → M15-04 (texture + sampler)
M15-01 → M15-05 (buffer management)
M15-02 + M15-03 + M15-04 + M15-05 → M15-06 (command encoder)
M15-01 → M15-07 (software rasterizer fallback)

M15-02 → M16-01 (EGSL compiler, dipende da PSO descriptor)
M16-01 → M16-02 (SPIR-V frontend)
M16-02 → M16-03 (backend AGX)
M16-02 → M16-04 (backend virgl QEMU)
M16-02 → M16-05 (shader cache)

M3-03 → M17-01 (mld daemon)
M17-01 → M17-02 (tensor abstraction)
M17-02 → M17-03 (ONNX loader)
M17-01 + M17-02 → M17-04 (pipeline unificata ANE+GPU)
M17-04 + M15-05 → M17-05 (neural bridge GPU↔ANE)

M15-06 → M18-01 (ray tracing)
M15-06 → M18-02 (indirect command buffer)
M15-06 → M18-03 (mesh shader)
M15-06 → M18-04 (VRS)

M17-05 + M18-01 → M19-01 (neural upscaling ANLSS)
M17-05 + M18-01 → M19-02 (AI denoising)
M17-05 + M18-04 → M19-03 (saliency VRS)
M17-05 + M15-04 → M19-04 (neural material synthesis)
```

---

## Prossimi tre step consigliati (inizio Backlog 3)

1. **M15-01** Device + Heap — fondamenta di EnlilGFX; si costruisce su GBO già esistenti di M5b-03
2. **M16-02** SPIR-V frontend + **M16-04** backend virgl — permette di caricare shader reali su QEMU
   senza attendere il compilatore AGX; sblocca lo sviluppo dell'intera pipeline su QEMU
3. **M17-01** `mld` daemon — wrappa M3-03 che è già completo; ha il ritorno AI immediato

Dopo M15-01..06 + M16-02..04: EnlilOS ha un renderer 3D completo su QEMU con SPIR-V.
Dopo M17-01..04: inferenza ONNX gira su ANE reale su Apple M-series, CPU su QEMU.
Dopo M18-01 + M19-01: pipeline completa rendering RT + denoising AI = qualità next-gen.
