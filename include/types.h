/*
 * NROS Microkernel - Tipi base
 */

#ifndef NROS_TYPES_H
#define NROS_TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

typedef uint64_t            size_t;
typedef int64_t             ssize_t;
typedef uint64_t            uintptr_t;

#define NULL ((void *)0)

#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
/* C23+ ha bool/true/false built-in */
#else
#define bool  _Bool
#define true  1
#define false 0
#endif
#endif

/* Accesso a registri memory-mapped */
#define MMIO_READ32(addr)       (*(volatile uint32_t *)(addr))
#define MMIO_WRITE32(addr, val) (*(volatile uint32_t *)(addr) = (val))

#endif
