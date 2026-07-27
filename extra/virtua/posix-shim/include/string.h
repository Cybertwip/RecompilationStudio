#ifndef _POSIX_SHIM_STRING_H
#define _POSIX_SHIM_STRING_H

#if defined(__has_include_next)
#  if __has_include_next(<string.h>)
#    include_next <string.h>
#  endif
#else
#  include_next <string.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char* left, const char* right);
int strncasecmp(const char* left, const char* right, size_t count);

#ifdef __cplusplus
}
#endif

#endif
