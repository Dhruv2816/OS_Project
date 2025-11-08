#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

/* Use 17.14 fixed-point format */
#define F (1 << 14)

/* Convert integer n to fixed-point */
#define INT_TO_FP(n) ((n) * F)

/* Convert fixed-point x to integer (rounding to zero) */
#define FP_TO_INT_ZERO(x) ((x) / F)

/* Convert fixed-point x to integer (rounding to nearest) */
#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + F / 2) / F : ((x) - F / 2) / F)

/* Add two fixed-point numbers */
#define FP_ADD_FP(x, y) ((x) + (y))

/* Subtract two fixed-point numbers */
#define FP_SUB_FP(x, y) ((x) - (y))

/* Add a fixed-point and an integer */
#define FP_ADD_INT(x, n) ((x) + (n) * F)

/* Subtract an integer from a fixed-point */
#define FP_SUB_INT(x, n) ((x) - (n) * F)

/* Multiply two fixed-point numbers */
/* Use int64_t to prevent overflow during intermediate multiplication */
#define FP_MUL_FP(x, y) ((((int64_t) (x)) * (y)) / F)

/* Multiply a fixed-point by an integer */
#define FP_MUL_INT(x, n) ((x) * (n))

/* Divide a fixed-point by a fixed-point */
/* Use int64_t to prevent overflow during intermediate multiplication */
#define FP_DIV_FP(x, y) ((((int64_t) (x)) * F) / (y))

/* Divide a fixed-point by an integer */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */