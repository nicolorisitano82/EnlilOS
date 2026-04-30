#ifndef ENLILOS_MUSL_MATH_H
#define ENLILOS_MUSL_MATH_H

#define HUGE_VAL (__builtin_huge_val())

static inline int isnan(double x)
{
    return x != x;
}

static inline int isinf(double x)
{
    return !isnan(x) && (x > 1.0e308 || x < -1.0e308);
}

static inline double pow(double base, double exp)
{
    long long iexp = (long long)exp;
    double    result = 1.0;
    int       negative;

    if ((double)iexp != exp)
        return 0.0;

    negative = (iexp < 0);
    if (negative)
        iexp = -iexp;

    while (iexp > 0) {
        if (iexp & 1LL)
            result *= base;
        base *= base;
        iexp >>= 1;
    }

    return negative ? (1.0 / result) : result;
}

#endif
