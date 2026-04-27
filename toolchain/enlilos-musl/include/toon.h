#ifndef ENLILOS_MUSL_TOON_H
#define ENLILOS_MUSL_TOON_H

#include <stddef.h>

#define TOON_MAX_SCALARS     32U
#define TOON_MAX_ARRAYS      8U
#define TOON_MAX_ARRAY_ITEMS 64U
#define TOON_MAX_STORAGE     4096U
#define TOON_MAX_ERROR       128U

typedef struct {
    const char *key;
    const char *value;
} toon_kv_t;

typedef struct {
    const char  *key;
    const char  *items[TOON_MAX_ARRAY_ITEMS];
    int          count;
} toon_arr_t;

typedef struct {
    toon_kv_t  scalars[TOON_MAX_SCALARS];
    int        nscalars;
    toon_arr_t arrays[TOON_MAX_ARRAYS];
    int        narrays;
    char       error[TOON_MAX_ERROR];
    char       storage[TOON_MAX_STORAGE];
    size_t     storage_len;
} toon_doc_t;

int          toon_parse(const char *src, size_t len, toon_doc_t *out);
int          toon_parse_file(const char *path, toon_doc_t *out);
const char  *toon_get_string(const toon_doc_t *doc, const char *key);
int          toon_get_bool(const toon_doc_t *doc, const char *key, int def);
const char **toon_get_array(const toon_doc_t *doc, const char *key, int *count_out);

#endif
