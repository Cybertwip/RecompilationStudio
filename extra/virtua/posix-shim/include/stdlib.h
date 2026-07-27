/*
 * MVII baremetal POSIX shim - <stdlib.h> additions used by libc++.
 */
#ifndef _POSIX_SHIM_STDLIB_H
#define _POSIX_SHIM_STDLIB_H

#include_next <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned int arc4random(void);
float strtof_l(const char* __restrict, char** __restrict, locale_t);
double strtod_l(const char* __restrict, char** __restrict, locale_t);
long double strtold_l(const char* __restrict, char** __restrict, locale_t);
char* realpath(const char* __restrict, char* __restrict);
int mkstemp(char*);
int setenv(const char*, const char*, int);
int unsetenv(const char*);
int putenv(char*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_STDLIB_H */
