#ifndef ENLILOS_MUSL_SETJMP_H
#define ENLILOS_MUSL_SETJMP_H

#include <stdint.h>

/*
 * Minimal AArch64 setjmp/longjmp ABI for EnlilOS user-space.
 *
 * We save the callee-saved integer registers x19-x30, SP and the
 * callee-saved FP/SIMD registers d8-d15. Signal mask preservation is not
 * implemented in v1, so sigsetjmp/siglongjmp are aliases of setjmp/longjmp.
 */

typedef uint64_t __jmp_buf[22];
typedef __jmp_buf jmp_buf;
typedef __jmp_buf sigjmp_buf;

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

static inline int sigsetjmp(sigjmp_buf env, int savemask)
{
    (void)savemask;
    return setjmp(env);
}

static inline void siglongjmp(sigjmp_buf env, int val)
{
    longjmp(env, val);
}

#define _setjmp(env)    setjmp((env))
#define _longjmp(env,v) longjmp((env),(v))

#endif
