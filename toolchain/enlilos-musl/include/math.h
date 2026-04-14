#ifndef ENLILOS_MUSL_MATH_H
#define ENLILOS_MUSL_MATH_H

static inline int isnan(double x)
{
    return x != x;
}

static inline int isinf(double x)
{
    return !isnan(x) && (x > 1.0e308 || x < -1.0e308);
}

#endif
