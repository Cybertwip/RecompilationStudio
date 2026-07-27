/*
 * MVII baremetal POSIX shim - <wctype.h> additions used by libc++.
 */
#ifndef _POSIX_SHIM_WCTYPE_H
#define _POSIX_SHIM_WCTYPE_H

#include_next <wctype.h>
#include <locale.h>

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _POSIX_SHIM_WCTYPE_T_DEFINED
#define _POSIX_SHIM_WCTYPE_T_DEFINED
typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;
#endif

int iswalnum(wint_t);
int iswblank(wint_t);
int iswcntrl(wint_t);
int iswdigit(wint_t);
int iswgraph(wint_t);
int iswlower(wint_t);
int iswprint(wint_t);
int iswpunct(wint_t);
int iswspace(wint_t);
int iswupper(wint_t);
int iswxdigit(wint_t);
int iswctype(wint_t, wctype_t);
wctype_t wctype(const char*);
wint_t towlower(wint_t);
wint_t towupper(wint_t);
wint_t towctrans(wint_t, wctrans_t);
wctrans_t wctrans(const char*);

int iswctype_l(wint_t, wctype_t, locale_t);
int iswspace_l(wint_t, locale_t);
int iswprint_l(wint_t, locale_t);
int iswcntrl_l(wint_t, locale_t);
int iswupper_l(wint_t, locale_t);
int iswlower_l(wint_t, locale_t);
int iswalpha_l(wint_t, locale_t);
int iswblank_l(wint_t, locale_t);
int iswdigit_l(wint_t, locale_t);
int iswpunct_l(wint_t, locale_t);
int iswxdigit_l(wint_t, locale_t);
wint_t towupper_l(wint_t, locale_t);
wint_t towlower_l(wint_t, locale_t);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_WCTYPE_H */
