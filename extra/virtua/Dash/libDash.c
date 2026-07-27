#include <libc.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#if defined(__has_include)
#  if __has_include(<dirent.h>)
#    include <dirent.h>
#  else
#    define VIRTUA_DASH_NEEDS_DIRENT_TYPES 1
#  endif
#  if __has_include(<sys/socket.h>)
#    include <sys/socket.h>
#  else
#    define VIRTUA_DASH_NEEDS_SOCKET_TYPES 1
#  endif
#else
#  include <dirent.h>
#  include <sys/socket.h>
#endif

#if defined(VIRTUA_DASH_NEEDS_DIRENT_TYPES)
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_REG 8
#define DT_DIR 4
#endif
typedef struct __virtua_dash_dir DIR;
struct dirent {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};
#define _DIRENT_HAVE_D_OFF 1
#define _DIRENT_HAVE_D_RECLEN 1
#define _DIRENT_HAVE_D_TYPE 1
#endif

#if defined(VIRTUA_DASH_NEEDS_SOCKET_TYPES)
typedef unsigned int socklen_t;
struct sockaddr;
#endif

#define UNUSED(x) (void)(x)

// Default buffer size
#define DEFAULT_BUF_SIZE 4096

// --- System Call Prototypes ---
int sys_open(const char *pathname, int flags, int mode);
int sys_close(int fd);
ssize_t sys_read(int fd, void *buf, size_t count);
ssize_t sys_write(int fd, const void *buf, size_t count);
off_t sys_lseek(int fd, off_t offset, int whence);
int sys_fstat(int fd, struct stat *statbuf);
int sys_mkdir(const char *pathname, int mode);
int sys_stat(const char *pathname, struct stat *statbuf);
int sys_unlink(const char *pathname);
long sys_getcwd(char *buf, size_t size);
int sys_chdir(const char *pathname);
long sys_opendir(const char *pathname);
long sys_readdir(long handle, void *out_dirent);
long sys_closedir(long handle);
long sys_socket(int domain, int type, int protocol);
long sys_connect(int fd, const void *addr, uint32_t addrlen);
long sys_bind(int fd, const void *addr, uint32_t addrlen);
long sys_listen(int fd, int backlog);
long sys_accept(int fd, void *addr, uint32_t *addrlen);
long sys_send(int fd, const void *buf, size_t count, int flags);
long sys_recv(int fd, void *buf, size_t count, int flags);
long sys_sendto(int fd, const void *buf, size_t count, int flags, const void *addr, uint32_t addrlen);
long sys_recvfrom(int fd, void *buf, size_t count, int flags, void *addr, uint32_t *addrlen);
long sys_getsockname(int fd, void *addr, uint32_t *addrlen);
long sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen);
long sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen);
long sys_shutdown(int fd, int how);
long sys_poll(void *fds, size_t nfds, int timeout_ms);
long sys_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout);

extern size_t brk(size_t addr);
extern int sys_exit(int code);

#ifndef DASH_FIXED_HEAP_BYTES
#define DASH_FIXED_HEAP_BYTES 0
#endif

int errno;
int *__errno(void) { return &errno; }

// ==========================================
// MEMORY & STRING HELPERS
// ==========================================

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;
    if (d == s) return dest;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

size_t strlen(const char *s) {
    size_t i = 0;
    while (s[i]) i++;
    return i;
}

char *strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}

// Minimal strtok
static char *strtok_pos = NULL;
char *strtok(char *str, const char *delim) {
    if (str) strtok_pos = str;
    if (!strtok_pos || !*strtok_pos) return NULL;
    while (*strtok_pos && strchr(delim, *strtok_pos)) strtok_pos++;
    if (!*strtok_pos) return NULL;
    char *start = strtok_pos;
    while (*strtok_pos && !strchr(delim, *strtok_pos)) strtok_pos++;
    if (*strtok_pos) {
        *strtok_pos = '\0';
        strtok_pos++;
    }
    return start;
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

// Added atof (critical for OBJ loading)
double atof(const char *s) {
    double a = 0.0;
    int e = 0;
    int c;
    while ((c = *s++) != '\0' && isdigit(c)) {
        a = a*10.0 + (c - '0');
    }
    if (c == '.') {
        while ((c = *s++) != '\0' && isdigit(c)) {
            a = a*10.0 + (c - '0');
            e = e-1;
        }
    }
    while (e > 0) { a *= 10.0; e--; }
    while (e < 0) { a *= 0.1; e++; }
    return a;
}

// ==========================================
// MEMORY ALLOCATOR
// ==========================================

#define MALLOC_ALIGN 16
#define ALIGNED_ALLOC_MAGIC 0x5649525455414c31ULL /* "VIRTUAL1" */
typedef long Align;
union header { struct { union header *ptr; size_t size; } s; Align x; };
typedef union header Header;
static Header base;
static Header *freep = NULL;

typedef struct AlignedAllocHeader {
    uint64_t magic;
    void *base;
} AlignedAllocHeader;

#if DASH_FIXED_HEAP_BYTES > 0
__attribute__((aligned(MALLOC_ALIGN)))
static unsigned char dash_fixed_heap[DASH_FIXED_HEAP_BYTES];
#endif

void *sbrk(ptrdiff_t increment) {
    static void *heap_end = NULL;
    static int use_fixed_heap = 0;
#if DASH_FIXED_HEAP_BYTES > 0
    unsigned char *fixed_base = dash_fixed_heap;
    unsigned char *fixed_limit = dash_fixed_heap + sizeof(dash_fixed_heap);
#else
    unsigned char *fixed_base = NULL;
    unsigned char *fixed_limit = NULL;
#endif

    if (heap_end == NULL) {
        if (fixed_base && fixed_limit && fixed_base < fixed_limit) {
            use_fixed_heap = 1;
            heap_end = (void *)fixed_base;
        } else {
            heap_end = (void *)brk(0);
        }

        uintptr_t addr = (uintptr_t)heap_end;
        if (addr % MALLOC_ALIGN != 0) {
            ptrdiff_t offset = MALLOC_ALIGN - (addr % MALLOC_ALIGN);
            if (use_fixed_heap) {
                if ((void *)(fixed_base + offset) > (void *)fixed_limit) {
                    errno = ENOMEM;
                    return (void *)-1;
                }
            } else {
                brk(addr + offset);
            }
            heap_end = (void *)(addr + offset);
        }
    }
    if (increment == 0) return heap_end;
    if (increment % MALLOC_ALIGN != 0) increment += MALLOC_ALIGN - (increment % MALLOC_ALIGN);
    uintptr_t new_brk = (uintptr_t)heap_end + increment;
    if (use_fixed_heap) {
        if ((unsigned char *)new_brk > fixed_limit) {
            errno = ENOMEM;
            return (void *)-1;
        }
    } else {
        size_t brk_ret = brk(new_brk);
        if (brk_ret == (size_t)-1) {
            printf("sbrk failed! increment=%ld, new_brk=%zu\\n", (long)increment, (size_t)new_brk);
            errno = 12;
            return (void *)-1;
        }
    }
    void *prev = heap_end;
    heap_end = (void *)new_brk;
    return prev;
}

static Header *morecore(size_t nu) {
    if (nu < 4096) nu = 4096;
    char *cp = (char *)sbrk(nu * sizeof(Header));
    if (cp == (char *)-1) return NULL;
    Header *up = (Header *)cp;
    up->s.size = nu;
    free((void *)(up + 1));
    return freep;
}

void *malloc(size_t nbytes) {
    if (nbytes == 0) return NULL;
    size_t nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
    Header *prevp, *p;
    if ((prevp = freep) == NULL) { base.s.ptr = freep = prevp = &base; base.s.size = 0; }
    for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
        if (p->s.size >= nunits) {
            if (p->s.size == nunits) prevp->s.ptr = p->s.ptr;
            else { p->s.size -= nunits; p += p->s.size; p->s.size = nunits; }
            freep = prevp;
            return (void *)(p + 1);
        }
        if (p == freep) if ((p = morecore(nunits)) == NULL) return NULL;
    }
}

void free(void *ap) {
    if (!ap) return;
    AlignedAllocHeader *aligned = (AlignedAllocHeader *)((unsigned char *)ap - sizeof(AlignedAllocHeader));
    if (aligned->magic == ALIGNED_ALLOC_MAGIC && aligned->base) {
        void *base_ptr = aligned->base;
        aligned->magic = 0;
        free(base_ptr);
        return;
    }

    Header *bp = (Header *)ap - 1, *p;
    for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
        if (p >= p->s.ptr && (bp > p || bp < p->s.ptr)) break;
    if (bp + bp->s.size == p->s.ptr) { bp->s.size += p->s.ptr->s.size; bp->s.ptr = p->s.ptr->s.ptr; }
    else bp->s.ptr = p->s.ptr;
    if (p + p->s.size == bp) { p->s.size += bp->s.size; p->s.ptr = bp->s.ptr; }
    else p->s.ptr = bp;
    freep = p;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    Header *bp = (Header *)ptr - 1;
    size_t old_size = (bp->s.size - 1) * sizeof(Header);
    if (size <= old_size) return ptr;
    void *new_ptr = malloc(size);
    if (new_ptr) { memcpy(new_ptr, ptr, old_size); free(ptr); }
    return new_ptr;
}

void *aligned_alloc(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    if (size == 0) {
        size = alignment;
    } else if (size % alignment != 0) {
        size += alignment - (size % alignment);
    }
    if (alignment <= MALLOC_ALIGN) {
        return malloc(size);
    }

    if (size > (size_t)-1 - alignment - sizeof(AlignedAllocHeader)) {
        errno = ENOMEM;
        return NULL;
    }

    void *raw = malloc(size + alignment - 1 + sizeof(AlignedAllocHeader));
    if (!raw) {
        errno = ENOMEM;
        return NULL;
    }

    uintptr_t start = (uintptr_t)raw + sizeof(AlignedAllocHeader);
    uintptr_t aligned_addr = (start + alignment - 1) & ~(uintptr_t)(alignment - 1);
    AlignedAllocHeader *header = (AlignedAllocHeader *)(aligned_addr - sizeof(AlignedAllocHeader));
    header->magic = ALIGNED_ALLOC_MAGIC;
    header->base = raw;
    return (void *)aligned_addr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!memptr) return EINVAL;
    *memptr = NULL;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void *ptr = aligned_alloc(alignment, size);
    if (!ptr) {
        return errno ? errno : ENOMEM;
    }
    *memptr = ptr;
    return 0;
}

// ==========================================
// IO WRAPPERS
// ==========================================

static int print_str(const char* s) { return sys_write(1, s, strlen(s)); }

int __attribute__((weak)) printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    print_str(format); 
    va_end(args);
    return 0;
}

int __attribute__((weak)) vsnprintf(char *str, size_t size, const char *format, va_list args) {
    if (!str || size == 0 || !format) {
        if (str && size > 0) str[0] = '\0';
        return format ? (int)strlen(format) : 0;
    }

    size_t out = 0;

#define PUTC(c) do { if (out + 1 < size) str[out] = (c); out++; } while (0)

    for (const char *p = format; *p; ++p) {
        if (*p != '%') {
            PUTC(*p);
            continue;
        }
        ++p;
        if (*p == '%') { PUTC('%'); continue; }

        /* flags */
        int left_justify = 0, zero_pad = 0;
        for (;;) {
            if (*p == '-') { left_justify = 1; ++p; }
            else if (*p == '0') { zero_pad = 1; ++p; }
            else if (*p == '+' || *p == ' ' || *p == '#') { ++p; }
            else break;
        }

        /* width */
        int width = 0;
        if (*p == '*') { width = va_arg(args, int); ++p; if (width < 0) { left_justify = 1; width = -width; } }
        else { while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); ++p; } }

        /* precision */
        int precision = -1;
        if (*p == '.') {
            ++p;
            if (*p == '*') { precision = va_arg(args, int); ++p; if (precision < 0) precision = -1; }
            else { precision = 0; while (*p >= '0' && *p <= '9') { precision = precision * 10 + (*p - '0'); ++p; } }
        }

        /* length modifier */
        int is_long = 0, is_longlong = 0, is_size = 0;
        if (*p == 'l') { ++p; if (*p == 'l') { is_longlong = 1; ++p; } else is_long = 1; }
        else if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        else if (*p == 'z') { is_size = 1; ++p; }
        else if (*p == 't' || *p == 'j') { ++p; }

        char conv = *p;

        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            unsigned long long uval;
            long long sval = 0;
            int is_neg = 0;
            int is_signed = (conv == 'd' || conv == 'i');
            unsigned base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o' ? 8 : 10);
            int upper = (conv == 'X');

            if (is_longlong) { if (is_signed) { long long v = va_arg(args, long long); sval = v; is_neg = v < 0; uval = is_neg ? (unsigned long long)(-(v+1))+1ULL : (unsigned long long)v; } else { uval = va_arg(args, unsigned long long); } }
            else if (is_size)  { size_t v = va_arg(args, size_t); uval = (unsigned long long)v; }
            else if (is_long)  { if (is_signed) { long v = va_arg(args, long); sval = v; is_neg = v < 0; uval = is_neg ? (unsigned long long)(-(v+1))+1ULL : (unsigned long long)v; } else { uval = va_arg(args, unsigned long); } }
            else               { if (is_signed) { int v = va_arg(args, int); sval = v; is_neg = v < 0; uval = is_neg ? (unsigned long long)(-(v+1))+1ULL : (unsigned long long)v; } else { uval = va_arg(args, unsigned int); } }

            (void)sval;

            const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[32]; int tlen = 0;
            unsigned long long tv = uval;
            do { tmp[tlen++] = digits[tv % base]; tv /= base; } while (tv && tlen < 32);

            /* precision = minimum digit count (zero-pad digits, not total width) */
            while (precision > tlen && tlen < 32) tmp[tlen++] = '0';

            int numlen = tlen + (is_neg ? 1 : 0);
            int pad = width > numlen ? width - numlen : 0;
            char pad_ch = (!left_justify && zero_pad) ? '0' : ' ';

            if (!left_justify) for (int i = 0; i < pad; i++) PUTC(pad_ch);
            if (is_neg) PUTC('-');
            for (int i = tlen - 1; i >= 0; i--) PUTC(tmp[i]);
            if (left_justify) for (int i = 0; i < pad; i++) PUTC(' ');

        } else if (conv == 's') {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            int slen = (int)strlen(s);
            if (precision >= 0 && slen > precision) slen = precision;
            int pad = width > slen ? width - slen : 0;
            if (!left_justify) for (int i = 0; i < pad; i++) PUTC(' ');
            for (int i = 0; i < slen; i++) PUTC(s[i]);
            if (left_justify) for (int i = 0; i < pad; i++) PUTC(' ');

        } else if (conv == 'c') {
            char c = (char)va_arg(args, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left_justify) for (int i = 0; i < pad; i++) PUTC(' ');
            PUTC(c);
            if (left_justify) for (int i = 0; i < pad; i++) PUTC(' ');

        } else if (conv == 'p') {
            uintptr_t ptr = (uintptr_t)va_arg(args, void *);
            PUTC('0'); PUTC('x');
            const char *hd = "0123456789abcdef";
            char tmp[16]; int tlen = 0;
            uintptr_t tv = ptr;
            do { tmp[tlen++] = hd[tv & 0xf]; tv >>= 4; } while (tv && tlen < 16);
            for (int i = tlen - 1; i >= 0; i--) PUTC(tmp[i]);

        } else {
            PUTC('%');
            if (conv) PUTC(conv);
        }
    }

#undef PUTC

    if (out < size) str[out] = '\0';
    else if (size > 0) str[size - 1] = '\0';
    return (int)out;
}

int __attribute__((weak)) snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int rc = vsnprintf(str, size, format, args);
    va_end(args);
    return rc;
}

int puts(const char *s) {
    if (sys_write(1, s, strlen(s)) < 0) return -1;
    sys_write(1, "\n", 1);
    return 1;
}

int _open(const char *name, int flags, int mode) {
    int ret = sys_open(name, flags, mode);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int _close(int file) {
    int ret = sys_close(file);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int _read(int file, char *ptr, int len) {
    ssize_t ret = sys_read(file, ptr, len);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int _write(int file, char *ptr, int len) {
    ssize_t ret = sys_write(file, ptr, len);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int _lseek(int file, int ptr, int dir) {
    off_t ret = sys_lseek(file, ptr, dir);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int _fstat(int file, struct stat *st) {
    int ret = sys_fstat(file, st);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// --- POSIX Wrappers ---

static int virtua_dash_tolower_ascii(int ch) {
    return (ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch;
}

int __attribute__((weak)) strcasecmp(const char *left, const char *right) {
    if (left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    while (*left && *right) {
        int lc = virtua_dash_tolower_ascii((unsigned char)*left++);
        int rc = virtua_dash_tolower_ascii((unsigned char)*right++);
        if (lc != rc) return lc - rc;
    }
    return virtua_dash_tolower_ascii((unsigned char)*left) - virtua_dash_tolower_ascii((unsigned char)*right);
}

int __attribute__((weak)) strncasecmp(const char *left, const char *right, size_t count) {
    if (count == 0 || left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    for (size_t i = 0; i < count; ++i) {
        int lc = virtua_dash_tolower_ascii((unsigned char)left[i]);
        int rc = virtua_dash_tolower_ascii((unsigned char)right[i]);
        if (lc != rc || lc == 0 || rc == 0) return lc - rc;
    }
    return 0;
}

static wchar_t virtua_dash_towlower_ascii(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') ? (wchar_t)(ch + (L'a' - L'A')) : ch;
}

int __attribute__((weak)) wcscasecmp(const wchar_t *left, const wchar_t *right) {
    if (left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    while (*left && *right) {
        wchar_t lc = virtua_dash_towlower_ascii(*left++);
        wchar_t rc = virtua_dash_towlower_ascii(*right++);
        if (lc != rc) return (lc < rc) ? -1 : 1;
    }
    wchar_t lc = virtua_dash_towlower_ascii(*left);
    wchar_t rc = virtua_dash_towlower_ascii(*right);
    if (lc == rc) return 0;
    return (lc < rc) ? -1 : 1;
}

int __attribute__((weak)) wcsncasecmp(const wchar_t *left, const wchar_t *right, size_t count) {
    if (count == 0 || left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    for (size_t i = 0; i < count; ++i) {
        wchar_t lc = virtua_dash_towlower_ascii(left[i]);
        wchar_t rc = virtua_dash_towlower_ascii(right[i]);
        if (lc != rc) return (lc < rc) ? -1 : 1;
        if (lc == 0) return 0;
    }
    return 0;
}

int __attribute__((weak)) open(const char *name, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return _open(name, flags, mode);
}

int __attribute__((weak)) close(int file) {
    return _close(file);
}

ssize_t __attribute__((weak)) read(int file, void *ptr, size_t len) {
    ssize_t ret = sys_read(file, ptr, len);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

ssize_t __attribute__((weak)) write(int file, const void *ptr, size_t len) {
    if (len > 64u * 1024u * 1024u) {
        char diagnostic[224];
        int diagnostic_len = snprintf(diagnostic, sizeof(diagnostic),
                                      "virtua-write: rejected fd=%d buf=%p len=%llu caller=%p self=%p\n",
                                      file, ptr, (unsigned long long)len, __builtin_return_address(0), (void *)&write);
        if (diagnostic_len > 0) sys_write(2, diagnostic, (size_t)diagnostic_len);
        errno = EINVAL;
        return -1;
    }
    ssize_t ret = sys_write(file, ptr, len);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

off_t __attribute__((weak)) lseek(int file, off_t ptr, int dir) {
    off_t ret = sys_lseek(file, ptr, dir);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

off64_t __attribute__((weak)) lseek64(int file, off64_t ptr, int dir) {
    return (off64_t)lseek(file, (off_t)ptr, dir);
}

int __attribute__((weak)) fstat(int file, struct stat *st) {
    return _fstat(file, st);
}

int __attribute__((weak)) stat(const char *name, struct stat *st) {
    int ret = sys_stat(name, st);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int __attribute__((weak)) unlink(const char *name) {
    int ret = sys_unlink(name);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

char __attribute__((weak)) *getcwd(char *buf, size_t size) {
    long ret = sys_getcwd(buf, size);
    if (ret < 0) {
        errno = (int)-ret;
        return NULL;
    }
    return buf;
}

int __attribute__((weak)) chdir(const char *name) {
    int ret = sys_chdir(name);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

typedef struct {
    long handle;
    struct dirent entry;
} VirtuaDashDIR;

DIR __attribute__((weak)) *opendir(const char *name) {
    long handle = sys_opendir(name);
    if (handle < 0) {
        errno = (int)-handle;
        return NULL;
    }
    VirtuaDashDIR *dir = (VirtuaDashDIR *)malloc(sizeof(VirtuaDashDIR));
    if (!dir) {
        sys_closedir(handle);
        errno = ENOMEM;
        return NULL;
    }
    memset(dir, 0, sizeof(*dir));
    dir->handle = handle;
    return (DIR *)dir;
}

struct virtua_dash_dirent_wire {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    uint8_t d_reserved;
    char d_name[256];
};

struct dirent __attribute__((weak)) *readdir(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return NULL;
    }
    VirtuaDashDIR *dir = (VirtuaDashDIR *)dirp;
    struct virtua_dash_dirent_wire wire;
    memset(&wire, 0, sizeof(wire));
    long ret = sys_readdir(dir->handle, &wire);
    if (ret == 0) return NULL;
    if (ret < 0) {
        errno = (int)-ret;
        return NULL;
    }
    memset(&dir->entry, 0, sizeof(dir->entry));
    dir->entry.d_ino = (unsigned long)wire.d_ino;
#if defined(_DIRENT_HAVE_D_OFF)
    dir->entry.d_off = (long)wire.d_off;
#endif
#if defined(_DIRENT_HAVE_D_RECLEN)
    dir->entry.d_reclen = wire.d_reclen;
#endif
#if defined(_DIRENT_HAVE_D_TYPE)
    dir->entry.d_type = wire.d_type;
#endif
    for (size_t i = 0; i + 1 < sizeof(dir->entry.d_name); ++i) {
        dir->entry.d_name[i] = wire.d_name[i];
        if (wire.d_name[i] == '\0') break;
    }
    dir->entry.d_name[sizeof(dir->entry.d_name) - 1] = '\0';
    return &dir->entry;
}

int __attribute__((weak)) closedir(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    VirtuaDashDIR *dir = (VirtuaDashDIR *)dirp;
    long ret = sys_closedir(dir->handle);
    free(dir);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

int _isatty(int file) {
    if (file == STDIN_FILENO || file == STDOUT_FILENO || file == STDERR_FILENO) {
        return 1;
    }
    return 0;
}

int _kill(int pid, int sig) {
    UNUSED(pid);
    UNUSED(sig);
    errno = EINVAL;
    return -1;
}

int _getpid(void) {
    return 1;
}

void _exit(int status) {
    sys_exit(status);
    while (1);
}



// Undefine standard macros to prevent conflicts with our implementations
#undef fopen
#undef fclose
#undef fread
#undef feof
#undef ferror

// ==========================================
// STDIO IMPLEMENTATION
// ==========================================

// Define our internal structure that holds the actual state.
// We cast FILE* to this struct internally.
typedef struct {
    int fd;                 // File descriptor
    int flags;              // Open flags
    unsigned char *buf;     // Pointer to the buffer
    size_t buf_cap;         // Buffer capacity (alloc size)
    size_t buf_len;         // Bytes currently in buffer
    size_t buf_pos;         // Current position in buffer
    int eof;                // EOF indicator
    int err;                // Error indicator
} _MyFILE;

// --- fopen ---
FILE * __attribute__((weak)) fopen(const char *filename, const char *mode) {
    if (!filename || !mode) {
        errno = EINVAL;
        return NULL;
    }

    int flags = 0;
    int access_mode = 0666; // rw-rw-rw-
    
    // Parse mode
    // r  - Open for reading
    // w  - Open for writing (truncate)
    // a  - Open for appending
    // +  - Update (read/write)
    
    int rw = 0;
    if (strchr(mode, '+')) rw = 1;

    if (*mode == 'r') {
        flags = rw ? O_RDWR : O_RDONLY;
    } else if (*mode == 'w') {
        flags = rw ? O_RDWR : O_WRONLY;
        flags |= (O_CREAT | O_TRUNC);
    } else if (*mode == 'a') {
        flags = rw ? O_RDWR : O_WRONLY;
        flags |= (O_CREAT | O_APPEND);
    } else {
        errno = EINVAL;
        return NULL;
    }

    // Attempt to open the file
    int fd = sys_open(filename, flags, access_mode);
    if (fd < 0) {
        errno = -fd;
        return NULL;
    }

    // Allocate our internal struct
    _MyFILE *stream = (_MyFILE *)malloc(sizeof(_MyFILE));
    if (!stream) {
        sys_close(fd);
        errno = ENOMEM;
        return NULL;
    }

    // Allocate the internal buffer
    stream->buf = (unsigned char *)malloc(DEFAULT_BUF_SIZE);
    if (!stream->buf) {
        sys_close(fd);
        free(stream);
        errno = ENOMEM;
        return NULL;
    }

    // Initialize members
    stream->fd = fd;
    stream->flags = flags;
    stream->buf_cap = DEFAULT_BUF_SIZE;
    stream->buf_len = 0;
    stream->buf_pos = 0;
    stream->eof = 0;
    stream->err = 0;

    // Cast our internal struct to FILE* to satisfy the return type
    return (FILE *)stream;
}

// --- fclose ---
int __attribute__((weak)) fclose(FILE *stream_ptr) {
    if (!stream_ptr) return EOF;

    // Cast opaque FILE* back to our internal struct
    _MyFILE *stream = (_MyFILE *)stream_ptr;

    int r = sys_close(stream->fd);
    int ret = 0;

    if (r < 0) {
        errno = -r;
        ret = EOF;
    }

    if (stream->buf) {
        free(stream->buf);
    }
    free(stream);

    return ret;
}

// --- feof ---
int __attribute__((weak)) feof(FILE *stream_ptr) {
    if (!stream_ptr) return 0;
    _MyFILE *stream = (_MyFILE *)stream_ptr;
    return stream->eof;
}

int __attribute__((weak)) ferror(FILE *stream_ptr) {
    if (!stream_ptr) return 0;
    _MyFILE *stream = (_MyFILE *)stream_ptr;
    return stream->err;
}

static off_t stream_tell(_MyFILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return (off_t)-1;
    }

    off_t kernel_pos = sys_lseek(stream->fd, 0, SEEK_CUR);
    if (kernel_pos < 0) {
        errno = (int)-kernel_pos;
        stream->err = 1;
        return (off_t)-1;
    }

    const off_t unread = (off_t)(stream->buf_len - stream->buf_pos);
    return kernel_pos - unread;
}

// --- fread ---
size_t __attribute__((weak)) fread(void *ptr, size_t size, size_t nmemb, FILE *stream_ptr) {
    if (!stream_ptr || !ptr || size == 0 || nmemb == 0) return 0;

    // Cast opaque FILE* back to our internal struct
    _MyFILE *stream = (_MyFILE *)stream_ptr;
    
    unsigned char *dest = (unsigned char *)ptr;
    size_t total_bytes = size * nmemb;
    size_t bytes_read = 0;

    while (bytes_read < total_bytes) {
        // Case 1: Use data already in the buffer
        if (stream->buf_pos < stream->buf_len) {
            size_t available = stream->buf_len - stream->buf_pos;
            size_t remaining = total_bytes - bytes_read;
            size_t copy_size = (remaining < available) ? remaining : available;

            memcpy(dest + bytes_read, stream->buf + stream->buf_pos, copy_size);
            
            stream->buf_pos += copy_size;
            bytes_read += copy_size;
        } 
        // Case 2: Buffer is empty, need to read from disk
        else {
            if (stream->eof || stream->err) break;

            size_t remaining = total_bytes - bytes_read;

            // Optimization: If request is larger than buffer capacity, 
            // read directly into user's memory to avoid double copy.
            if (remaining >= stream->buf_cap) {
                ssize_t ret = sys_read(stream->fd, dest + bytes_read, remaining);
                if (ret == 0) {
                    stream->eof = 1;
                    break;
                } else if (ret < 0) {
                    stream->err = 1;
                    break;
                }
                bytes_read += ret;
            } 
            // Normal buffering: Fill the internal buffer
            else {
                ssize_t ret = sys_read(stream->fd, stream->buf, stream->buf_cap);
                if (ret == 0) {
                    stream->eof = 1;
                    break;
                } else if (ret < 0) {
                    stream->err = 1;
                    break;
                }
                stream->buf_len = ret;
                stream->buf_pos = 0;
            }
        }
    }

    return bytes_read / size;
}

int __attribute__((weak)) fseek(FILE *stream_ptr, long offset, int whence) {
    if (!stream_ptr) {
        errno = EINVAL;
        return -1;
    }

    _MyFILE *stream = (_MyFILE *)stream_ptr;
    off_t target = 0;

    if (whence == SEEK_SET) {
        target = (off_t)offset;
    } else if (whence == SEEK_CUR) {
        const off_t current = stream_tell(stream);
        if (current < 0) return -1;
        target = current + (off_t)offset;
    } else if (whence == SEEK_END) {
        struct stat st;
        if (sys_fstat(stream->fd, &st) < 0) {
            errno = EIO;
            stream->err = 1;
            return -1;
        }
        target = st.st_size + (off_t)offset;
    } else {
        errno = EINVAL;
        return -1;
    }

    const off_t rc = sys_lseek(stream->fd, target, SEEK_SET);
    if (rc < 0) {
        errno = (int)-rc;
        stream->err = 1;
        return -1;
    }

    stream->buf_len = 0;
    stream->buf_pos = 0;
    stream->eof = 0;
    return 0;
}

long __attribute__((weak)) ftell(FILE *stream_ptr) {
    const off_t pos = stream_tell((_MyFILE *)stream_ptr);
    if (pos < 0) return -1L;
    return (long)pos;
}

void __attribute__((weak)) rewind(FILE *stream_ptr) {
    if (!stream_ptr) return;
    _MyFILE *stream = (_MyFILE *)stream_ptr;
    (void)fseek(stream_ptr, 0, SEEK_SET);
    stream->err = 0;
    stream->eof = 0;
}

int __attribute__((weak)) fflush(FILE *stream_ptr) {
    (void)stream_ptr;
    return 0;
}

int __attribute__((weak)) fgetc(FILE *stream_ptr) {
    unsigned char c = 0;
    return fread(&c, 1, 1, stream_ptr) == 1 ? (int)c : EOF;
}

int __attribute__((weak)) getc(FILE *stream_ptr) {
    return fgetc(stream_ptr);
}

int __attribute__((weak)) ungetc(int c, FILE *stream_ptr) {
    if (!stream_ptr || c == EOF) return EOF;

    _MyFILE *stream = (_MyFILE *)stream_ptr;
    if (stream->buf_pos > 0) {
        stream->buf[--stream->buf_pos] = (unsigned char)c;
        stream->eof = 0;
        return c;
    }

    if (stream->buf_len < stream->buf_cap) {
        memmove(stream->buf + 1, stream->buf, stream->buf_len);
        stream->buf[0] = (unsigned char)c;
        stream->buf_len += 1;
        stream->buf_pos = 0;
        stream->eof = 0;
        return c;
    }

    return EOF;
}

int mkdir(const char *pathname, mode_t mode) { return sys_mkdir(pathname, mode); }

int __attribute__((weak)) socket(int domain, int type, int protocol) {
    long ret = sys_socket(domain, type, protocol);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    long ret = sys_connect(fd, addr, (uint32_t)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    long ret = sys_bind(fd, addr, (uint32_t)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) listen(int fd, int backlog) {
    long ret = sys_listen(fd, backlog);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    uint32_t wire_len = addrlen ? (uint32_t)*addrlen : 0;
    long ret = sys_accept(fd, addr, addrlen ? &wire_len : NULL);
    if (addrlen) *addrlen = (socklen_t)wire_len;
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

ssize_t __attribute__((weak)) send(int fd, const void *buf, size_t count, int flags) {
    long ret = sys_send(fd, buf, count, flags);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

ssize_t __attribute__((weak)) recv(int fd, void *buf, size_t count, int flags) {
    long ret = sys_recv(fd, buf, count, flags);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

ssize_t __attribute__((weak)) sendto(int fd, const void *buf, size_t count, int flags,
                                     const struct sockaddr *addr, socklen_t addrlen) {
    long ret = sys_sendto(fd, buf, count, flags, addr, (uint32_t)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

ssize_t __attribute__((weak)) recvfrom(int fd, void *buf, size_t count, int flags,
                                       struct sockaddr *addr, socklen_t *addrlen) {
    uint32_t wire_len = addrlen ? (uint32_t)*addrlen : 0;
    long ret = sys_recvfrom(fd, buf, count, flags, addr, addrlen ? &wire_len : NULL);
    if (addrlen) *addrlen = (socklen_t)wire_len;
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

int __attribute__((weak)) getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    uint32_t wire_len = addrlen ? (uint32_t)*addrlen : 0;
    long ret = sys_getsockname(fd, addr, addrlen ? &wire_len : NULL);
    if (addrlen) *addrlen = (socklen_t)wire_len;
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    long ret = sys_setsockopt(fd, level, optname, optval, (uint32_t)optlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    uint32_t wire_len = optlen ? (uint32_t)*optlen : 0;
    long ret = sys_getsockopt(fd, level, optname, optval, optlen ? &wire_len : NULL);
    if (optlen) *optlen = (socklen_t)wire_len;
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) shutdown(int fd, int how) {
    long ret = sys_shutdown(fd, how);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    long ret = sys_poll(fds, (size_t)nfds, timeout_ms);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int __attribute__((weak)) select(int nfds,
                                 fd_set *readfds,
                                 fd_set *writefds,
                                 fd_set *exceptfds,
                                 struct timeval *timeout) {
    long ret = sys_select(nfds, readfds, writefds, exceptfds, timeout);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

void* __dso_handle = 0;
