/* vfxtest.h -- three-macro test harness. No framework, no dependency. */
#ifndef VFX_TEST_H
#define VFX_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int vfx_test_failures;
static int vfx_test_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++vfx_test_checks;                                                   \
        if (!(cond)) {                                                       \
            ++vfx_test_failures;                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                     \
                    __FILE__, __LINE__, #cond);                              \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                \
    do {                                                                     \
        double va_ = (double)(a), vb_ = (double)(b), t_ = (double)(tol);     \
        ++vfx_test_checks;                                                   \
        if (!(fabs(va_ - vb_) <= t_)) {                                      \
            ++vfx_test_failures;                                             \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s (%g) vs %s (%g),"  \
                    " tol %g\n", __FILE__, __LINE__, #a, va_, #b, vb_, t_);  \
        }                                                                    \
    } while (0)

#define TEST_MAIN_END()                                                      \
    do {                                                                     \
        printf("%d checks, %d failures\n", vfx_test_checks, vfx_test_failures); \
        return vfx_test_failures ? 1 : 0;                                    \
    } while (0)

#endif /* VFX_TEST_H */
