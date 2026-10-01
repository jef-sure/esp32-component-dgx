#pragma once
#include <stdio.h>
#include <stdlib.h>

static int check_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++check_failures;                                                    \
        }                                                                        \
    } while (0)

#define CHECK_DONE()                                                             \
    do {                                                                         \
        if (check_failures) {                                                    \
            fprintf(stderr, "%s: %d check(s) failed\n", __FILE__, check_failures); \
            return EXIT_FAILURE;                                                 \
        }                                                                        \
        printf("%s: ok\n", __FILE__);                                            \
        return EXIT_SUCCESS;                                                     \
    } while (0)
