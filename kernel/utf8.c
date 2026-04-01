/*
 * EnlilOS Microkernel - UTF-8 Decoder (M4-04)
 *
 * Decodifica sequenze UTF-8 multi-byte in codepoint Unicode.
 *
 * Standard: RFC 3629 (UTF-8, Unicode 6.0+)
 *
 * Struttura sequenze valide:
 *   1 byte : 0xxxxxxx                         (U+0000–U+007F)
 *   2 byte : 110xxxxx 10xxxxxx                 (U+0080–U+07FF)
 *   3 byte : 1110xxxx 10xxxxxx 10xxxxxx        (U+0800–U+FFFF)
 *   4 byte : 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx (U+10000–U+10FFFF)
 *
 * Casi di errore → U+FFFD, avanza di 1 byte:
 *   - Byte di continuazione inatteso (10xxxxxx all'inizio)
 *   - Continuation byte mancante o errato
 *   - Overlong encoding
 *   - Codepoint > U+10FFFF o surrogate pair UTF-16 (U+D800–U+DFFF)
 */

#include "utf8.h"

uint32_t utf8_decode(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint8_t b0 = *p;
    uint32_t cp;

    if (b0 == 0x00) {
        /* Terminatore stringa — non avanzare */
        return 0;
    }

    /* ASCII (U+0000–U+007F) */
    if (b0 < 0x80U) {
        *s += 1;
        return (uint32_t)b0;
    }

    /* Continuation byte come primo byte — malformato */
    if (b0 < 0xC0U) {
        *s += 1;
        return UTF8_REPLACEMENT;
    }

    /* 2 byte (U+0080–U+07FF) */
    if (b0 < 0xE0U) {
        uint8_t b1 = p[1];
        if ((b1 & 0xC0U) != 0x80U) { *s += 1; return UTF8_REPLACEMENT; }
        cp = ((uint32_t)(b0 & 0x1FU) << 6) | (uint32_t)(b1 & 0x3FU);
        if (cp < 0x80U) { *s += 2; return UTF8_REPLACEMENT; } /* overlong */
        *s += 2;
        return cp;
    }

    /* 3 byte (U+0800–U+FFFF) */
    if (b0 < 0xF0U) {
        uint8_t b1 = p[1];
        uint8_t b2 = p[2];
        if ((b1 & 0xC0U) != 0x80U) { *s += 1; return UTF8_REPLACEMENT; }
        if ((b2 & 0xC0U) != 0x80U) { *s += 2; return UTF8_REPLACEMENT; }
        cp = ((uint32_t)(b0 & 0x0FU) << 12)
           | ((uint32_t)(b1 & 0x3FU) <<  6)
           |  (uint32_t)(b2 & 0x3FU);
        if (cp < 0x800U)             { *s += 3; return UTF8_REPLACEMENT; } /* overlong */
        if (cp >= 0xD800U && cp <= 0xDFFFU) { *s += 3; return UTF8_REPLACEMENT; } /* surrogate */
        *s += 3;
        return cp;
    }

    /* 4 byte (U+10000–U+10FFFF) */
    if (b0 < 0xF8U) {
        uint8_t b1 = p[1];
        uint8_t b2 = p[2];
        uint8_t b3 = p[3];
        if ((b1 & 0xC0U) != 0x80U) { *s += 1; return UTF8_REPLACEMENT; }
        if ((b2 & 0xC0U) != 0x80U) { *s += 2; return UTF8_REPLACEMENT; }
        if ((b3 & 0xC0U) != 0x80U) { *s += 3; return UTF8_REPLACEMENT; }
        cp = ((uint32_t)(b0 & 0x07U) << 18)
           | ((uint32_t)(b1 & 0x3FU) << 12)
           | ((uint32_t)(b2 & 0x3FU) <<  6)
           |  (uint32_t)(b3 & 0x3FU);
        if (cp < 0x10000U || cp > 0x10FFFFU) { *s += 4; return UTF8_REPLACEMENT; }
        *s += 4;
        return cp;
    }

    /* Byte 0xF8..0xFF non valido in UTF-8 */
    *s += 1;
    return UTF8_REPLACEMENT;
}

uint32_t utf8_strlen(const char *s)
{
    uint32_t count = 0;
    while (*s) {
        utf8_decode(&s);
        count++;
    }
    return count;
}
