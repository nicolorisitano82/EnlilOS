/*
 * NROS Microkernel - Framebuffer Driver
 *
 * Utilizza un framebuffer lineare in memoria.
 * Per QEMU virt con -device virtio-gpu, il framebuffer viene
 * configurato tramite fw_cfg. Per semplicità, usiamo un buffer
 * in memoria e lo rendiamo accessibile tramite il dispositivo
 * ramfb di QEMU.
 *
 * In un microkernel completo, questo sarebbe un server user-space.
 */

#include "framebuffer.h"
#include "uart.h"
#include "mmu.h"

/* Font bitmap 8x16 - caratteri ASCII stampabili (32-126) */
/* Font minimale incorporato nel kernel per il boot */
static const uint8_t font_8x16[][16] = {
    /* Spazio (32) */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* ! (33) */
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* " (34) */
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* # (35) */
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00},
    /* Placeholder $ % & ' ( ) * + , (33-41 → idx 4-12) */
    [4 ... 12] = {0x00,0x00,0x7E,0x81,0x81,0x81,0x81,0x81,0x81,0x81,0x81,0x7E,0x00,0x00,0x00,0x00},
    /* - (45 → idx 13) */
    [13] = {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* . (46 → idx 14) */
    [14] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* / (47 → idx 15) */
    [15] = {0x00,0x00,0x06,0x06,0x0C,0x0C,0x18,0x18,0x30,0x30,0x60,0x60,0x00,0x00,0x00,0x00},
    /* 0 (48 → idx 16) */
    [16] = {0x00,0x00,0x7C,0xC6,0xCE,0xDE,0xD6,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 1 (49 → idx 17) */
    [17] = {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    /* 2 (50 → idx 18) */
    [18] = {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 3 (51 → idx 19) */
    [19] = {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 4 (52 → idx 20) */
    [20] = {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    /* 5 (53 → idx 21) */
    [21] = {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 6 (54 → idx 22) */
    [22] = {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 7 (55 → idx 23) */
    [23] = {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 8 (56 → idx 24) */
    [24] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 9 (57 → idx 25) */
    [25] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    /* : ; < = > ? @ (58-64 → idx 26-32) */
    [26 ... 32] = {0x00,0x00,0x7E,0x81,0x81,0x81,0x81,0x81,0x81,0x81,0x81,0x7E,0x00,0x00,0x00,0x00},
    /* A (65) */
    [33] = {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* B */
    [34] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    /* C */
    [35] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* D */
    [36] = {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    /* E */
    [37] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* F */
    [38] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* G */
    [39] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    /* H */
    [40] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* I */
    [41] = {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* J */
    [42] = {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* K */
    [43] = {0x00,0x00,0xE6,0x66,0x6C,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* L */
    [44] = {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* M */
    [45] = {0x00,0x00,0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* N */
    [46] = {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* O */
    [47] = {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /* P */
    [48] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* Q */
    [49] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00,0x00},
    /* R */
    [50] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* S */
    [51] = {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* T */
    [52] = {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* U */
    [53] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* V */
    [54] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    /* W */
    [55] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    /* X */
    [56] = {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* Y */
    [57] = {0x00,0x00,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* Z */
    [58] = {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* [ - ` : 91-96 */
    [59 ... 64] = {0x00,0x00,0x7E,0x81,0x81,0x81,0x81,0x81,0x81,0x81,0x81,0x7E,0x00,0x00,0x00,0x00},
};

/* Framebuffer in memoria - usato come buffer lineare */
static uint32_t framebuffer[FB_WIDTH * FB_HEIGHT] __attribute__((aligned(4096)));

/* Puntatore al framebuffer attivo */
static volatile uint32_t *fb_ptr;

/*
 * Inizializza il framebuffer.
 * Per QEMU virt senza GPU, usiamo un framebuffer in RAM
 * e lo esponiamo tramite fw_cfg/ramfb.
 *
 * Per il testing iniziale, il framebuffer è un'area di memoria
 * che verrà letta dal dispositivo ramfb di QEMU.
 */

/* QEMU fw_cfg MMIO interface (virt machine) */
#define FW_CFG_BASE         0x09020000
#define FW_CFG_DATA         (FW_CFG_BASE + 0x00)  /* 8-bit data read/write */
#define FW_CFG_SELECTOR     (FW_CFG_BASE + 0x08)  /* 16-bit selector (BE) */
#define FW_CFG_DMA_ADDR     (FW_CFG_BASE + 0x10)  /* 64-bit DMA address (BE) */

/* fw_cfg well-known selectors */
#define FW_CFG_FILE_DIR     0x0019

/* fw_cfg DMA control bits */
#define FW_CFG_DMA_CTL_ERROR    0x01
#define FW_CFG_DMA_CTL_READ     0x02
#define FW_CFG_DMA_CTL_SKIP     0x04
#define FW_CFG_DMA_CTL_SELECT   0x08
#define FW_CFG_DMA_CTL_WRITE    0x10

/* ramfb config structure (28 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} ramfb_cfg_t;

/* fw_cfg file directory entry */
typedef struct __attribute__((packed)) {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char     name[56];
} fw_cfg_file_t;

/* Byte swap helpers per big-endian fw_cfg */
static uint32_t bswap32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

static uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)v) << 32) |
           (uint64_t)bswap32((uint32_t)(v >> 32));
}

static uint16_t bswap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

/* Leggi un byte dal data register (porta 8-bit) */
static uint8_t fw_cfg_read8(void)
{
    return (uint8_t)(*(volatile uint8_t *)(FW_CFG_DATA));
}

/* Seleziona un file fw_cfg */
static void fw_cfg_select(uint16_t key)
{
    *(volatile uint16_t *)(FW_CFG_SELECTOR) = bswap16(key);
}

/* Leggi N byte dal fw_cfg data port */
static void fw_cfg_read_bytes(void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        p[i] = fw_cfg_read8();
    }
}

/*
 * fw_cfg_dma_write — scrive 'len' byte dal buffer fisico 'data_pa'
 * nel file fw_cfg identificato da 'sel', usando l'interfaccia DMA.
 *
 * Questa è l'unica modalità di scrittura supportata in modo affidabile
 * su QEMU virt. La scrittura byte-per-byte al DATA register richiede
 * FW_CFG_WRITE_CHANNEL (0x4000) nel selettore, che non sempre funziona.
 *
 * Protocollo:
 *   1. Prepara QEMUCFGDMAAccess in RAM (buffer statico, allineato a 8B)
 *   2. DC CIVAC sul buffer dati  → QEMU DMA vede i nostri dati
 *   3. DC CIVAC sulla struct DMA → QEMU DMA vede la richiesta
 *   4. Scrivi PA della struct a FW_CFG_DMA_ADDR (64-bit big-endian)
 *   5. Poll: DC IVAC + leggi control fino a (control & ~ERROR) == 0
 */
typedef struct __attribute__((packed, aligned(8))) {
    uint32_t control;   /* BE: [31:16]=selector [4]=WRITE [3]=SELECT [1]=READ */
    uint32_t length;    /* BE: byte da trasferire */
    uint64_t address;   /* BE: indirizzo fisico del buffer dati */
} fw_cfg_dma_t;

static fw_cfg_dma_t  fw_dma_req;           /* deve stare in RAM, non stack */
static ramfb_cfg_t   fw_ramfb_cfg_buf;     /* idem */

static void fw_cfg_dma_write(uint16_t sel, const void *data, uint32_t len)
{
    /* Copia dati nel buffer statico in RAM */
    const uint8_t *src = (const uint8_t *)data;
    uint8_t       *dst = (uint8_t *)&fw_ramfb_cfg_buf;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    /* Prepara la richiesta DMA */
    fw_dma_req.control = bswap32(FW_CFG_DMA_CTL_SELECT |
                                 FW_CFG_DMA_CTL_WRITE  |
                                 ((uint32_t)sel << 16));
    fw_dma_req.length  = bswap32(len);
    fw_dma_req.address = bswap64((uint64_t)(uintptr_t)&fw_ramfb_cfg_buf);

    /* Flush D-cache: QEMU DMA legge dalla RAM fisica, non dalla cache CPU */
    cache_flush_range((uintptr_t)&fw_ramfb_cfg_buf, len);
    cache_flush_range((uintptr_t)&fw_dma_req,        sizeof(fw_dma_req));

    /* Trigger: scrivi PA della struct DMA in FW_CFG_DMA_ADDR (BE, high first) */
    uint64_t req_pa = (uint64_t)(uintptr_t)&fw_dma_req;
    MMIO_WRITE32(FW_CFG_DMA_ADDR,     bswap32((uint32_t)(req_pa >> 32)));
    MMIO_WRITE32(FW_CFG_DMA_ADDR + 4, bswap32((uint32_t)(req_pa)));

    /* Poll: aspetta che QEMU azzeri il campo control (o setti ERROR)
     * DC IVAC prima di ogni lettura: invalida la cache senza writeback
     * così leggiamo il valore aggiornato da QEMU nella RAM fisica.     */
    for (uint32_t retry = 0; retry < 100000; retry++) {
        __asm__ volatile("dc ivac, %0" :: "r"((uintptr_t)&fw_dma_req) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");

        uint32_t ctrl = bswap32(fw_dma_req.control);
        if ((ctrl & ~FW_CFG_DMA_CTL_ERROR) == 0) {
            if (ctrl & FW_CFG_DMA_CTL_ERROR)
                uart_puts("[FB] fw_cfg DMA: errore segnalato da QEMU\n");
            return;
        }
    }
    uart_puts("[FB] fw_cfg DMA: timeout\n");
}

/* Confronto stringhe inline */
static int fw_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

/*
 * Cerca un file nel fw_cfg directory e ritorna il suo selector.
 * Ritorna 0 se non trovato.
 */
static uint16_t fw_cfg_find_file(const char *name)
{
    fw_cfg_select(FW_CFG_FILE_DIR);

    /* Il primo uint32_t (BE) è il numero di file */
    uint32_t count_be;
    fw_cfg_read_bytes(&count_be, 4);
    uint32_t count = bswap32(count_be);

    uart_puts("[NROS] fw_cfg: ");
    /* Stampa count come decimale semplice */
    char num[12];
    int pos = 0;
    uint32_t tmp = count;
    if (tmp == 0) { num[pos++] = '0'; }
    else {
        char rev[12];
        int rp = 0;
        while (tmp > 0) { rev[rp++] = '0' + (tmp % 10); tmp /= 10; }
        while (rp > 0) { num[pos++] = rev[--rp]; }
    }
    num[pos] = '\0';
    uart_puts(num);
    uart_puts(" file nel directory\n");

    for (uint32_t i = 0; i < count; i++) {
        fw_cfg_file_t entry;
        fw_cfg_read_bytes(&entry, sizeof(fw_cfg_file_t));

        if (fw_strcmp(entry.name, name) == 0) {
            uint16_t sel = bswap16(entry.select);
            uart_puts("[NROS] fw_cfg: trovato '");
            uart_puts(name);
            uart_puts("'\n");
            return sel;
        }
    }

    uart_puts("[NROS] fw_cfg: '");
    uart_puts(name);
    uart_puts("' NON trovato\n");
    return 0;
}

void fb_init(void)
{
    fb_ptr = framebuffer;

    /* Trova il selector per etc/ramfb nel fw_cfg directory */
    uint16_t ramfb_sel = fw_cfg_find_file("etc/ramfb");
    if (ramfb_sel == 0) {
        uart_puts("[NROS] ERRORE: ramfb non disponibile. Avvia QEMU con -device ramfb\n");
        return;
    }

    /* Prepara la configurazione ramfb (big-endian) nel buffer statico */
    ramfb_cfg_t cfg;
    cfg.addr   = bswap64((uint64_t)(uintptr_t)framebuffer);
    cfg.fourcc = bswap32(0x34325258); /* XR24 = XRGB8888 */
    cfg.flags  = 0;
    cfg.width  = bswap32(FB_WIDTH);
    cfg.height = bswap32(FB_HEIGHT);
    cfg.stride = bswap32(FB_WIDTH * FB_BPP);

    /* Scrivi tramite DMA fw_cfg (unica modalità affidabile su QEMU virt) */
    fw_cfg_dma_write(ramfb_sel, &cfg, sizeof(cfg));

    /* Pulisci lo schermo e scrivi in memoria fisica prima che QEMU legga */
    fb_clear(0x001a1a2e);
    fb_flush();

    uart_puts("[NROS] Framebuffer inizializzato: 800x600x32\n");
}

void fb_flush(void)
{
    cache_flush_range((uintptr_t)framebuffer,
                      FB_WIDTH * FB_HEIGHT * FB_BPP);
}

void fb_clear(uint32_t color)
{
    for (uint32_t i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        fb_ptr[i] = color;
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x < FB_WIDTH && y < FB_HEIGHT) {
        fb_ptr[y * FB_WIDTH + x] = color;
    }
}

void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    int idx;

    if (c < 32 || c > 90) {
        /* Carattere fuori range font: usa spazio (indice 0) */
        idx = 0;
    } else {
        idx = (int)(unsigned char)c - 32;
    }

    /* Limita all'array disponibile */
    if (idx < 0 || idx >= (int)(sizeof(font_8x16) / sizeof(font_8x16[0])))
        idx = 0;

    const uint8_t *glyph = font_8x16[idx];

    for (uint32_t row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

void fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg)
{
    uint32_t cx = x;
    while (*s) {
        fb_draw_char(cx, y, *s, fg, bg);
        cx += 8;
        s++;
    }
}

static uint32_t strlen_simple(const char *s)
{
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

void fb_draw_string_centered(const char *s, uint32_t fg, uint32_t bg)
{
    uint32_t len = strlen_simple(s);
    uint32_t text_width = len * 8;   /* 8 pixel per carattere */
    uint32_t text_height = 16;       /* 16 pixel per riga */

    uint32_t x = (FB_WIDTH - text_width) / 2;
    uint32_t y = (FB_HEIGHT - text_height) / 2;

    fb_draw_string(x, y, s, fg, bg);
}
