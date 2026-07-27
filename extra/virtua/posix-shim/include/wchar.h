/*
 * MVII baremetal POSIX shim - <wchar.h> additions used by libc++.
 */
#ifndef _POSIX_SHIM_WCHAR_H
#define _POSIX_SHIM_WCHAR_H

#include_next <wchar.h>
#include <locale.h>
#include <stdarg.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int swprintf(wchar_t* __restrict, size_t, const wchar_t* __restrict, ...);
int vswprintf(wchar_t* __restrict, size_t, const wchar_t* __restrict, va_list);
int fwprintf(FILE* __restrict, const wchar_t* __restrict, ...);
int vfwprintf(FILE* __restrict, const wchar_t* __restrict, va_list);
int wprintf(const wchar_t* __restrict, ...);
int vwprintf(const wchar_t* __restrict, va_list);
int fwscanf(FILE* __restrict, const wchar_t* __restrict, ...);
int vfwscanf(FILE* __restrict, const wchar_t* __restrict, va_list);
int swscanf(const wchar_t* __restrict, const wchar_t* __restrict, ...);
int vswscanf(const wchar_t* __restrict, const wchar_t* __restrict, va_list);
int wscanf(const wchar_t* __restrict, ...);
int vwscanf(const wchar_t* __restrict, va_list);
wint_t fgetwc(FILE*);
wchar_t* fgetws(wchar_t* __restrict, int, FILE* __restrict);
wint_t fputwc(wchar_t, FILE*);
int fputws(const wchar_t* __restrict, FILE* __restrict);
int fwide(FILE*, int);
wint_t getwc(FILE*);
wint_t getwchar(void);
wint_t putwc(wchar_t, FILE*);
wint_t putwchar(wchar_t);
wint_t ungetwc(wint_t, FILE*);
size_t wcsftime(wchar_t* __restrict, size_t, const wchar_t* __restrict, const struct tm* __restrict);
int wcscoll(const wchar_t*, const wchar_t*);
size_t wcsxfrm(wchar_t* __restrict, const wchar_t* __restrict, size_t);
int wcscoll_l(const wchar_t*, const wchar_t*, locale_t);
size_t wcsxfrm_l(wchar_t* __restrict, const wchar_t* __restrict, size_t, locale_t);
int wcscasecmp(const wchar_t*, const wchar_t*);
int wcsncasecmp(const wchar_t*, const wchar_t*, size_t);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_WCHAR_H */
