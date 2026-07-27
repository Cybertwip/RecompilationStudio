#pragma once 
#include <stdarg.h>
#include <stdint.h> // Provides size_t
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

// Declarations for memory functions
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);
void *memmove(void *dest, const void *src, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif