/*
 * MVII baremetal POSIX shim - minimal <dlfcn.h>.
 */
#ifndef _POSIX_SHIM_DLFCN_H
#define _POSIX_SHIM_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RTLD_LAZY
#define RTLD_LAZY 1
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 2
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0x100
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void*)0)
#endif

typedef struct {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
} Dl_info;

void* dlopen(const char*, int);
void* dlsym(void*, const char*);
int dlclose(void*);
char* dlerror(void);
int dladdr(const void*, Dl_info*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_DLFCN_H */
