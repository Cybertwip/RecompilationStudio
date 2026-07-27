// Minimal stubs for the platform-integration symbols that llvm-libc baremetal
// expects (used for both RISC-V and Machine64/x86_64 Virtua user binaries).
// These exist purely so that any libc TUs the linker pulls in (e.g. fopen,
// exit paths, __llvm_libc_* hooks) can resolve.  All real I/O + exit still go
// through Virtua's Dash syscalls (minos_user_abi + sys_* wrappers).

#include <time.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fenv.h>
#include <fcntl.h>
#include <ftw.h>
#include <ifaddrs.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <spawn.h>
#include <poll.h>
#include <pwd.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#ifdef snprintf
#undef snprintf
#endif
#ifdef printf
#undef printf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef fseek
#undef fseek
#endif
#ifdef ftell
#undef ftell
#endif
#ifdef fflush
#undef fflush
#endif
#ifdef fwrite
#undef fwrite
#endif
#ifdef getc
#undef getc
#endif
#ifdef sscanf
#undef sscanf
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef ferror
#undef ferror
#endif
#ifdef fgetc
#undef fgetc
#endif
#ifdef rewind
#undef rewind
#endif

#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VIRTUA_WEAK_SYMBOL __attribute__((weak))
#else
#define VIRTUA_WEAK_SYMBOL
#endif

extern "C" {
struct __processor_model {
    unsigned int __cpu_vendor;
    unsigned int __cpu_type;
    unsigned int __cpu_subtype;
    unsigned int __cpu_features[1];
};

VIRTUA_WEAK_SYMBOL __processor_model __cpu_model {};

VIRTUA_WEAK_SYMBOL int __cpu_indicator_init() {
    return 0;
}
}

extern "C" long sys_write(int fd, const void* buf, size_t count);
extern "C" long sys_read(int fd, void* buf, size_t count);
extern "C" long sys_lseek(int fd, long offset, int whence);
extern "C" long sys_stat(const char* path, struct stat* st);
extern "C" long sys_unlink(const char* path);
extern "C" long sys_rename(const char* oldpath, const char* newpath);
extern "C" long sys_getcwd(char* buf, size_t size);
extern "C" long sys_chdir(const char* path);
extern "C" long sys_pipe2(int* fds, int flags);
extern "C" long sys_dup3(int oldfd, int newfd, int flags);
extern "C" long sys_poll(void* fds, size_t nfds, int timeout_ms);
extern "C" long sys_select(int nfds, void* readfds, void* writefds, void* exceptfds, void* timeout);
extern "C" long sys_pwrite(int fd, const void* buf, size_t count, long offset);
extern "C" long sys_thread_create(uintptr_t* thread, uintptr_t entry, uintptr_t arg, size_t stack_size);
extern "C" long sys_thread_join(uintptr_t thread, void** retval);
extern "C" long sys_thread_detach(uintptr_t thread);
extern "C" uintptr_t sys_thread_self(void);
extern "C" long sys_thread_yield(void);
extern "C" long sys_sleep_until_us(uint64_t deadline_us);
extern "C" void sys_exit(int code);

namespace {

struct VirtuaCxaEhGlobals {
    void* caughtExceptions;
    unsigned int uncaughtExceptions;
};

constexpr unsigned int kMaxVirtuaThreads = 64;
VirtuaCxaEhGlobals g_cxa_eh_globals[kMaxVirtuaThreads] {};

static unsigned int current_thread_slot() {
    const uintptr_t thread = sys_thread_self();
    const uintptr_t normalized_thread = thread == 0 ? 1 : thread;
    return static_cast<unsigned int>(normalized_thread % kMaxVirtuaThreads);
}

} // namespace

extern "C" void* __wrap___cxa_get_globals() {
    return &g_cxa_eh_globals[current_thread_slot()];
}

extern "C" void* __wrap___cxa_get_globals_fast() {
    return &g_cxa_eh_globals[current_thread_slot()];
}

// x86 user binaries cannot trust raw rdtsc as microseconds: the kernel
// calibrates a per-boot TSC divisor (see minos_qemu_monotonic_microseconds
// in OS/MVII/Kernel/Machine64/Shared/Drivers/syscalls.cpp).  Routing the
// monotonic clock through sys_gettimeofday keeps Virtua user-space and the
// kernel in lockstep on time — otherwise a non-1GHz TSC makes Virtua's
// frame pacing diverge from kernel-driven Engine pacing.
struct minos_virtua_timeval {
    long tv_sec;
    long tv_usec;
};
extern "C" long sys_gettimeofday(void* tv, void* tz);

struct __llvm_libc_stdio_cookie {
    int fd;
};

namespace {

constexpr int                kTimespecGetMonotonicBase = 2;
constexpr unsigned long long kMicrosPerSecond          = 1000000ULL;
constexpr unsigned long long kNanosPerSecond           = 1000000000ULL;
constexpr unsigned long long kTimerHz                  = 10000000ULL;

#ifndef CLOCK_REALTIME
constexpr int kClockRealtime = 0;
#else
constexpr int kClockRealtime = CLOCK_REALTIME;
#endif
#ifndef CLOCK_MONOTONIC
constexpr int kClockMonotonic = 1;
#else
constexpr int kClockMonotonic = CLOCK_MONOTONIC;
#endif
#ifndef CLOCK_MONOTONIC_RAW
constexpr int kClockMonotonicRaw = kClockMonotonic;
#else
constexpr int kClockMonotonicRaw = CLOCK_MONOTONIC_RAW;
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(__arm__) || defined(__aarch64__)
#define VIRTUA_TIME_TICKS_ARE_MICROS 1
#endif

static inline unsigned long long kernel_monotonic_microseconds() {
    minos_virtua_timeval tv{0, 0};
    if (sys_gettimeofday(&tv, nullptr) != 0) return 0;
    return static_cast<unsigned long long>(tv.tv_sec) * kMicrosPerSecond +
           static_cast<unsigned long long>(tv.tv_usec);
}

static inline unsigned long long read_time_ticks() {
#if defined(__riscv)
    unsigned long long value = 0;
    __asm__ volatile("rdtime %0" : "=r"(value));
    return value;
#elif defined(VIRTUA_TIME_TICKS_ARE_MICROS)
    // Microsecond ticks from the kernel; these codepaths below treat
    // read_time_ticks() as microseconds (see fill_timespec_from_ticks).
    return kernel_monotonic_microseconds();
#else
    return 0;
#endif
}

[[noreturn]] void spin_after_exit() {
    for (;;) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#elif defined(__arm__) || defined(__aarch64__)
        __asm__ volatile("yield" ::: "memory");
#else
        __asm__ volatile("" ::: "memory");
#endif
    }
}

static int compile_month(const char* text) {
    return (text[0] == 'J' && text[1] == 'a') ? 1
         : (text[0] == 'F')                   ? 2
         : (text[0] == 'M' && text[2] == 'r') ? 3
         : (text[0] == 'A' && text[1] == 'p') ? 4
         : (text[0] == 'M')                   ? 5
         : (text[0] == 'J' && text[2] == 'n') ? 6
         : (text[0] == 'J')                   ? 7
         : (text[0] == 'A')                   ? 8
         : (text[0] == 'S')                   ? 9
         : (text[0] == 'O')                   ? 10
         : (text[0] == 'N')                   ? 11
                                               : 12;
}

static int parse_day(const char* text) {
    const int tens = text[0] == ' ' ? 0 : (text[0] - '0');
    return (tens * 10) + (text[1] - '0');
}

static int parse_int(const char* text, int digits) {
    int value = 0;

    for (int i = 0; i < digits; ++i) {
        value = (value * 10) + (text[i] - '0');
    }

    return value;
}

static long long days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

static void civil_from_days(long long days, int& year, unsigned& month, unsigned& day) {
    days += 719468LL;
    const long long era = (days >= 0 ? days : days - 146096LL) / 146097LL;
    const unsigned doe = static_cast<unsigned>(days - era * 146097LL);
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const unsigned mp = (5U * doy + 2U) / 153U;
    day = doy - (153U * mp + 2U) / 5U + 1U;
    month = mp + (mp < 10U ? 3U : static_cast<unsigned>(-9));
    year += month <= 2U;
}

static long long compile_time_epoch_seconds() {
    const int year = parse_int(&__DATE__[7], 4);
    const int month = compile_month(__DATE__);
    const int day = parse_day(&__DATE__[4]);
    const int hour = parse_int(&__TIME__[0], 2);
    const int minute = parse_int(&__TIME__[3], 2);
    const int second = parse_int(&__TIME__[6], 2);

    return (days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400LL)
        + (hour * 3600LL)
        + (minute * 60LL)
        + second;
}

static unsigned long long monotonic_microseconds() {
#if defined(VIRTUA_TIME_TICKS_ARE_MICROS)
    // read_time_ticks() already returns microseconds via sys_gettimeofday.
    return read_time_ticks();
#else
    return (read_time_ticks() * kMicrosPerSecond) / kTimerHz;
#endif
}

static int fill_timespec_from_ticks(struct timespec* ts, unsigned long long ticks) {
    if (ts == nullptr) {
        return 0;
    }

#if defined(VIRTUA_TIME_TICKS_ARE_MICROS)
    ts->tv_sec = static_cast<time_t>(ticks / kMicrosPerSecond);
    ts->tv_nsec = static_cast<long>((ticks % kMicrosPerSecond) * 1000ULL);
#else
    ts->tv_sec = static_cast<time_t>(ticks / kTimerHz);
    ts->tv_nsec = static_cast<long>(((ticks % kTimerHz) * kNanosPerSecond) / kTimerHz);
#endif
    return 1;
}

// Seconds at 2001-01-01. A box would have to have been powered on for thirty
// years for its uptime to reach this, so anything at or above it came from a
// real wall clock and anything below it is uptime.
constexpr unsigned long long kEpochPlausibilityFloorSeconds = 978307200ULL;

// Wall-clock microseconds.
//
// sys_gettimeofday() answers one of two different things and does not say
// which: a real date, when the kernel found one (an RTC, or the date MVII
// persists through sysprefs), or plain time since boot when it did not. Adding
// the build date unconditionally -- which is what this used to do -- is right
// only in the second case; in the first it stacks two epochs, and the ~113-year
// result overflows a 32-bit time_t into a *negative* tv_sec. Every std::chrono
// system_clock time point in the guest then goes negative with it, and anything
// that computed an absolute deadline from one waited effectively forever.
static unsigned long long realtime_microseconds() {
    const unsigned long long kernel_us = kernel_monotonic_microseconds();
    if (kernel_us / kMicrosPerSecond >= kEpochPlausibilityFloorSeconds) return kernel_us;
    static const long long kBaseEpoch = compile_time_epoch_seconds();
    return static_cast<unsigned long long>(kBaseEpoch) * kMicrosPerSecond + kernel_us;
}

static int fill_timespec_from_epoch_usec(struct timespec* ts, unsigned long long usec) {
    if (ts == nullptr) {
        return 0;
    }

    ts->tv_sec = static_cast<time_t>(usec / kMicrosPerSecond);
    ts->tv_nsec = static_cast<long>((usec % kMicrosPerSecond) * 1000ULL);
    return 1;
}

static inline int ascii_lower(int ch) {
    return (ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch;
}

static inline int ascii_upper(int ch) {
    return (ch >= 'a' && ch <= 'z') ? (ch - ('a' - 'A')) : ch;
}

static int parse_wctype_name(const char* property) {
    if (!property) return 0;
    if (strcmp(property, "alnum") == 0) return 1;
    if (strcmp(property, "alpha") == 0) return 2;
    if (strcmp(property, "blank") == 0) return 3;
    if (strcmp(property, "cntrl") == 0) return 4;
    if (strcmp(property, "digit") == 0) return 5;
    if (strcmp(property, "graph") == 0) return 6;
    if (strcmp(property, "lower") == 0) return 7;
    if (strcmp(property, "print") == 0) return 8;
    if (strcmp(property, "punct") == 0) return 9;
    if (strcmp(property, "space") == 0) return 10;
    if (strcmp(property, "upper") == 0) return 11;
    if (strcmp(property, "xdigit") == 0) return 12;
    return 0;
}

static int ascii_iswctype(wint_t ch, wctype_t desc) {
    switch (desc) {
    case 1: return iswalnum(ch);
    case 2: return iswalpha(ch);
    case 3: return iswblank(ch);
    case 4: return iswcntrl(ch);
    case 5: return iswdigit(ch);
    case 6: return iswgraph(ch);
    case 7: return iswlower(ch);
    case 8: return iswprint(ch);
    case 9: return iswpunct(ch);
    case 10: return iswspace(ch);
    case 11: return iswupper(ch);
    case 12: return iswxdigit(ch);
    default: return 0;
    }
}

int timespec_get_impl(struct timespec* ts, int base) {
    if (base == kTimespecGetMonotonicBase) {
        return fill_timespec_from_ticks(ts, read_time_ticks()) ? base : 0;
    }

    if (base == TIME_UTC) {
        return fill_timespec_from_epoch_usec(ts, realtime_microseconds()) ? base : 0;
    }

    return 0;
}

static struct tm* fill_utc_tm(time_t value, struct tm* out) {
    if (!out) return nullptr;
    long long seconds = static_cast<long long>(value);
    long long days = seconds / 86400LL;
    long long rem = seconds % 86400LL;
    if (rem < 0) {
        rem += 86400LL;
        --days;
    }

    int year = 1970;
    unsigned month = 1;
    unsigned mday = 1;
    civil_from_days(days, year, month, mday);
    memset(out, 0, sizeof(*out));
    out->tm_year = year - 1900;
    out->tm_mon = static_cast<int>(month) - 1;
    out->tm_mday = static_cast<int>(mday);
    out->tm_hour = static_cast<int>(rem / 3600LL);
    out->tm_min = static_cast<int>((rem / 60LL) % 60LL);
    out->tm_sec = static_cast<int>(rem % 60LL);
    out->tm_wday = static_cast<int>((days + 4LL) % 7LL);
    if (out->tm_wday < 0) out->tm_wday += 7;
    out->tm_yday = static_cast<int>(days - days_from_civil(year, 1, 1));
    out->tm_isdst = 0;
    return out;
}

static bool append_char(char*& out, size_t& remaining, char ch) {
    if (remaining <= 1) return false;
    *out++ = ch;
    --remaining;
    return true;
}

static bool append_text(char*& out, size_t& remaining, const char* text) {
    while (text && *text) {
        if (!append_char(out, remaining, *text++)) return false;
    }
    return true;
}

static bool append_decimal(char*& out, size_t& remaining, int value, int width) {
    char buffer[16];
    bool negative = value < 0;
    unsigned v = negative ? static_cast<unsigned>(-value) : static_cast<unsigned>(value);
    int pos = 0;
    do {
        buffer[pos++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    } while (v && pos < static_cast<int>(sizeof(buffer)));
    if (negative && pos < static_cast<int>(sizeof(buffer))) buffer[pos++] = '-';
    while (pos < width && pos < static_cast<int>(sizeof(buffer))) buffer[pos++] = '0';
    while (pos > 0) {
        if (!append_char(out, remaining, buffer[--pos])) return false;
    }
    return true;
}

static double parse_float(const char* text, char** endptr) {
    const char* p = text ? text : "";
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') ++p;
    double sign = 1.0;
    if (*p == '-') {
        sign = -1.0;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    double value = 0.0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        any = true;
        value = value * 10.0 + static_cast<double>(*p - '0');
        ++p;
    }
    if (*p == '.') {
        ++p;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            any = true;
            value += static_cast<double>(*p - '0') * scale;
            scale *= 0.1;
            ++p;
        }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char* exp_start = p++;
        int exp_sign = 1;
        if (*p == '-') {
            exp_sign = -1;
            ++p;
        } else if (*p == '+') {
            ++p;
        }
        int exponent = 0;
        bool exp_any = false;
        while (*p >= '0' && *p <= '9') {
            exp_any = true;
            exponent = exponent * 10 + (*p - '0');
            ++p;
        }
        if (exp_any) {
            double factor = 1.0;
            for (int i = 0; i < exponent; ++i) factor *= 10.0;
            value = exp_sign > 0 ? value * factor : value / factor;
        } else {
            p = exp_start;
        }
    }
    if (endptr) *endptr = const_cast<char*>(any ? p : text);
    return sign * value;
}

struct VirtuaDashFile {
    int fd;
    int flags;
    unsigned char* buf;
    size_t buf_cap;
    size_t buf_len;
    size_t buf_pos;
    int eof;
    int err;
};

constexpr unsigned int kMaxTlsKeys = 64;

struct TlsSlot {
    bool used;
    void (*destructor)(void*);
    void* values[kMaxVirtuaThreads];
};

TlsSlot g_tls[kMaxTlsKeys];

static int stream_fd(FILE* stream) {
    if (!stream) return -1;
    if (stream == stdin) return 0;
    if (stream == stdout) return 1;
    if (stream == stderr) return 2;
    return reinterpret_cast<VirtuaDashFile*>(stream)->fd;
}

static bool is_scan_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static const char* skip_scan_space(const char* cursor) {
    while (cursor && is_scan_space(*cursor)) ++cursor;
    return cursor;
}

static long long parse_scan_integer(const char*& cursor, int base, bool* parsed) {
    cursor = skip_scan_space(cursor);
    const char* p = cursor;
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        ++p;
    } else if (*p == '+') {
        ++p;
    }
    if (base == 0) {
        base = 10;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
        }
    }

    long long value = 0;
    bool any = false;
    for (;;) {
        int digit = -1;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = 10 + (*p - 'a');
        } else if (*p >= 'A' && *p <= 'F') {
            digit = 10 + (*p - 'A');
        }
        if (digit < 0 || digit >= base) break;
        any = true;
        value = value * base + digit;
        ++p;
    }
    if (parsed) *parsed = any;
    if (any) cursor = p;
    return value * sign;
}

static size_t encode_ascii_wchar(char* out, wchar_t ch) {
    if (!out) return 1;
    out[0] = (ch >= 0 && ch < 128) ? static_cast<char>(ch) : '?';
    return 1;
}

static size_t decode_ascii_wchar(wchar_t* out, const char* in, size_t in_size) {
    if (!in) return 0;
    if (in_size == 0) return static_cast<size_t>(-2);
    unsigned char ch = static_cast<unsigned char>(*in);
    if (out) *out = static_cast<wchar_t>(ch);
    return ch == 0 ? 0 : 1;
}

static void ensure_mutex(pthread_mutex_t* mutex) {
    if (mutex && !mutex->initialized) {
        mutex->initialized = 1;
        mutex->type = PTHREAD_MUTEX_NORMAL;
        mutex->locked = 0;
        mutex->recursion = 0;
        mutex->owner = 0;
    }
}

static uint64_t timespec_to_us(const struct timespec* ts) {
    if (!ts) return 0;
    return static_cast<uint64_t>(ts->tv_sec) * kMicrosPerSecond + static_cast<uint64_t>(ts->tv_nsec / 1000);
}

static uint64_t clamped_deadline_us(uint64_t start_us, uint64_t duration_us) {
    return duration_us > UINT64_MAX - start_us ? UINT64_MAX : start_us + duration_us;
}

// Absolute deadlines arrive from libstdc++ in the CLOCK_REALTIME domain (that is
// what std::condition_variable::wait_for builds them from) while everything that
// waits on them here reads the kernel clock, which is uptime unless the box has
// a persisted date. Comparing the two directly is off by the whole epoch -- a
// timed wait then never expires, and the thread sits in its retry loop for what
// works out to about fifty-six years. Re-express the deadline as "this far from
// now" and re-anchor it on the clock the waiter actually polls, which is exact
// whichever thing sys_gettimeofday() happens to be answering.
static uint64_t realtime_deadline_to_kernel_us(uint64_t deadline_realtime_us) {
    if (deadline_realtime_us == 0) return 0;
    const uint64_t now_kernel_us = kernel_monotonic_microseconds();
    const uint64_t now_realtime_us = realtime_microseconds();
    if (deadline_realtime_us <= now_realtime_us) return now_kernel_us;
    return clamped_deadline_us(now_kernel_us, deadline_realtime_us - now_realtime_us);
}

static void sleep_or_yield_until(uint64_t deadline_us) {
    if (deadline_us != 0 && sys_sleep_until_us(deadline_us) == 0) return;
    sched_yield();
}

static void sleep_or_yield_briefly(uint64_t absolute_deadline_us = 0) {
    const uint64_t now = kernel_monotonic_microseconds();
    if (now == 0) {
        sched_yield();
        return;
    }
    uint64_t deadline_us = now + 1000ULL;
    if (absolute_deadline_us != 0 && absolute_deadline_us < deadline_us) {
        deadline_us = absolute_deadline_us;
    }
    sleep_or_yield_until(deadline_us);
}

static uint16_t byteswap16(uint16_t value) {
    return static_cast<uint16_t>((value << 8) | (value >> 8));
}

static uint32_t byteswap32(uint32_t value) {
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

static bool parse_decimal_byte(const char*& cursor, unsigned& out) {
    if (!cursor || *cursor < '0' || *cursor > '9') return false;
    unsigned value = 0;
    unsigned digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10U + static_cast<unsigned>(*cursor - '0');
        if (value > 255U) return false;
        ++cursor;
        ++digits;
    }
    out = value;
    return digits > 0;
}

static bool parse_ipv4_address(const char* text, uint8_t out[4]) {
    if (!text || !*text) return false;
    const char* cursor = text;
    for (int i = 0; i < 4; ++i) {
        unsigned value = 0;
        if (!parse_decimal_byte(cursor, value)) return false;
        out[i] = static_cast<uint8_t>(value);
        if (i < 3) {
            if (*cursor != '.') return false;
            ++cursor;
        }
    }
    return *cursor == '\0';
}

static bool parse_ipv6_loopback_or_any(const char* text, uint8_t out[16]) {
    if (!text) return false;
    memset(out, 0, 16);
    if (strcmp(text, "::") == 0) {
        return true;
    }
    if (strcmp(text, "::1") == 0) {
        out[15] = 1;
        return true;
    }
    return false;
}

static bool parse_service_port(const char* service, uint16_t& port) {
    if (!service || !*service) {
        port = 0;
        return true;
    }
    unsigned value = 0;
    for (const char* cursor = service; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        value = (value * 10U) + static_cast<unsigned>(*cursor - '0');
        if (value > 65535U) return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

static char* duplicate_c_string(const char* text) {
    if (!text) return nullptr;
    const size_t len = strlen(text);
    char* copy = static_cast<char*>(malloc(len + 1));
    if (!copy) return nullptr;
    memcpy(copy, text, len + 1);
    return copy;
}

static bool write_text(char* out, size_t out_size, const char* text) {
    if (!out || out_size == 0 || !text) return false;
    const size_t len = strlen(text);
    if (len + 1 > out_size) return false;
    memcpy(out, text, len + 1);
    return true;
}

static void write_hex_uintptr(uintptr_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    char                  buffer[2 + sizeof(uintptr_t) * 2];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (size_t i = 0; i < sizeof(uintptr_t) * 2; ++i) {
        const size_t shift = (sizeof(uintptr_t) * 2 - 1 - i) * 4;
        buffer[2 + i]     = kHex[(value >> shift) & 0xfU];
    }
    sys_write(2, buffer, sizeof(buffer));
}

static void write_labeled_return_address(const char* label, size_t label_len, void* return_address) {
    sys_write(2, label, label_len);
    write_hex_uintptr(reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(return_address)));
}

static void write_abort_report(const char* reason,
                               size_t      reason_len,
                               void*       return_address0,
                               void*       return_address1,
                               void*       return_address2) {
    sys_write(2, reason, reason_len);
    static const char ra0_prefix[] = " ra0=";
    write_labeled_return_address(ra0_prefix, sizeof(ra0_prefix) - 1, return_address0);
    if (return_address1) {
        static const char ra1_prefix[] = " ra1=";
        write_labeled_return_address(ra1_prefix, sizeof(ra1_prefix) - 1, return_address1);
    }
    if (return_address2) {
        static const char ra2_prefix[] = " ra2=";
        write_labeled_return_address(ra2_prefix, sizeof(ra2_prefix) - 1, return_address2);
    }
    static const char newline[] = "\n";
    sys_write(2, newline, sizeof(newline) - 1);
}

struct VirtuaAddrinfoNode {
    struct addrinfo info;
    struct sockaddr_storage storage;
    char canonname[64];
};

#if defined(__SIZEOF_INT128__)
using VirtuaU128 = unsigned __int128;
using VirtuaS128 = __int128;

static VirtuaU128 divmod_u128(VirtuaU128 numerator, VirtuaU128 denominator, VirtuaU128* remainder) {
    if (denominator == 0) {
        abort();
    }

    VirtuaU128 quotient = 0;
    VirtuaU128 rem = 0;
    for (int bit = 127; bit >= 0; --bit) {
        rem = (rem << 1) | ((numerator >> bit) & 1);
        if (rem >= denominator) {
            rem -= denominator;
            quotient |= (static_cast<VirtuaU128>(1) << bit);
        }
    }
    if (remainder) {
        *remainder = rem;
    }
    return quotient;
}

static VirtuaU128 abs_i128(VirtuaS128 value) {
    const VirtuaU128 raw = static_cast<VirtuaU128>(value);
    return value < 0 ? (static_cast<VirtuaU128>(0) - raw) : raw;
}
#endif

#if defined(__arm__)
static uint32_t divmod_u32(uint32_t numerator, uint32_t denominator, uint32_t* remainder) {
    if (denominator == 0) {
        if (remainder) *remainder = numerator;
        return 0;
    }

    uint32_t quotient = 0;
    uint32_t rem      = 0;
    for (int bit = 31; bit >= 0; --bit) {
        rem = (rem << 1) | ((numerator >> bit) & 1U);
        if (rem >= denominator) {
            rem -= denominator;
            quotient |= (1U << bit);
        }
    }
    if (remainder) {
        *remainder = rem;
    }
    return quotient;
}

static uint64_t divmod_u64(uint64_t numerator, uint64_t denominator, uint64_t* remainder) {
    if (denominator == 0) {
        if (remainder) *remainder = numerator;
        return 0;
    }

    uint64_t quotient = 0;
    uint64_t rem      = 0;
    for (int bit = 63; bit >= 0; --bit) {
        rem = (rem << 1) | ((numerator >> bit) & 1ULL);
        if (rem >= denominator) {
            rem -= denominator;
            quotient |= (1ULL << bit);
        }
    }
    if (remainder) {
        *remainder = rem;
    }
    return quotient;
}

static uint32_t abs_i32_to_u32(int32_t value) {
    const uint32_t raw = static_cast<uint32_t>(value);
    return value < 0 ? (0U - raw) : raw;
}

static uint64_t abs_i64_to_u64(int64_t value) {
    const uint64_t raw = static_cast<uint64_t>(value);
    return value < 0 ? (0ULL - raw) : raw;
}

static int32_t apply_i32_sign(uint32_t value, bool negative) {
    return static_cast<int32_t>(negative ? (0U - value) : value);
}

static int64_t apply_i64_sign(uint64_t value, bool negative) {
    return static_cast<int64_t>(negative ? (0ULL - value) : value);
}

static uint64_t double_to_u64(double value) {
    union {
        double   d;
        uint64_t u;
    } bits {value};

    const bool     negative = (bits.u >> 63) != 0;
    const uint64_t exponent = (bits.u >> 52) & 0x7ffULL;
    uint64_t       mantissa = bits.u & ((1ULL << 52) - 1ULL);
    if (negative || exponent == 0) return 0;
    if (exponent == 0x7ffULL) return UINT64_MAX;

    const int unbiased_exponent = static_cast<int>(exponent) - 1023;
    if (unbiased_exponent < 0) return 0;
    if (unbiased_exponent >= 64) return UINT64_MAX;

    mantissa |= (1ULL << 52);
    return unbiased_exponent >= 52 ? (mantissa << (unbiased_exponent - 52))
                                   : (mantissa >> (52 - unbiased_exponent));
}

static int64_t double_to_i64(double value) {
    union {
        double   d;
        uint64_t u;
    } bits {value};

    const bool     negative = (bits.u >> 63) != 0;
    const uint64_t exponent = (bits.u >> 52) & 0x7ffULL;
    uint64_t       mantissa = bits.u & ((1ULL << 52) - 1ULL);
    if (exponent == 0) return 0;
    if (exponent == 0x7ffULL) return negative ? INT64_MIN : INT64_MAX;

    const int unbiased_exponent = static_cast<int>(exponent) - 1023;
    if (unbiased_exponent < 0) return 0;
    if (unbiased_exponent >= 63) return negative ? INT64_MIN : INT64_MAX;

    mantissa |= (1ULL << 52);
    const uint64_t magnitude = unbiased_exponent >= 52 ? (mantissa << (unbiased_exponent - 52))
                                                       : (mantissa >> (52 - unbiased_exponent));
    return apply_i64_sign(magnitude, negative);
}

static uint64_t float_to_u64(float value) {
    union {
        float    f;
        uint32_t u;
    } bits {value};

    const bool     negative = (bits.u >> 31) != 0;
    const uint32_t exponent = (bits.u >> 23) & 0xffU;
    uint64_t       mantissa = bits.u & ((1U << 23) - 1U);
    if (negative || exponent == 0) return 0;
    if (exponent == 0xffU) return UINT64_MAX;

    const int unbiased_exponent = static_cast<int>(exponent) - 127;
    if (unbiased_exponent < 0) return 0;
    if (unbiased_exponent >= 64) return UINT64_MAX;

    mantissa |= (1ULL << 23);
    return unbiased_exponent >= 23 ? (mantissa << (unbiased_exponent - 23))
                                   : (mantissa >> (23 - unbiased_exponent));
}

static int64_t float_to_i64(float value) {
    union {
        float    f;
        uint32_t u;
    } bits {value};

    const bool     negative = (bits.u >> 31) != 0;
    const uint32_t exponent = (bits.u >> 23) & 0xffU;
    uint64_t       mantissa = bits.u & ((1U << 23) - 1U);
    if (exponent == 0) return 0;
    if (exponent == 0xffU) return negative ? INT64_MIN : INT64_MAX;

    const int unbiased_exponent = static_cast<int>(exponent) - 127;
    if (unbiased_exponent < 0) return 0;
    if (unbiased_exponent >= 63) return negative ? INT64_MIN : INT64_MAX;

    mantissa |= (1ULL << 23);
    const uint64_t magnitude = unbiased_exponent >= 23 ? (mantissa << (unbiased_exponent - 23))
                                                       : (mantissa >> (23 - unbiased_exponent));
    return apply_i64_sign(magnitude, negative);
}

static double u64_to_double(uint64_t value) {
    const double high = static_cast<double>(static_cast<uint32_t>(value >> 32));
    const double low  = static_cast<double>(static_cast<uint32_t>(value));
    return high * 4294967296.0 + low;
}

static double i64_to_double(int64_t value) {
    const uint64_t magnitude = abs_i64_to_u64(value);
    const double   result    = u64_to_double(magnitude);
    return value < 0 ? -result : result;
}
#endif

} // namespace

extern "C" {

char* __virtua_empty_environ[] = {nullptr};
char** environ = __virtua_empty_environ;

[[noreturn]] void abort(void) {
    static const char message[] = "[virtua] abort()";
    void* const ra1 = __builtin_frame_address(1) ? __builtin_return_address(1) : nullptr;
    void* const ra2 = __builtin_frame_address(2) ? __builtin_return_address(2) : nullptr;
    write_abort_report(message, sizeof(message) - 1, __builtin_return_address(0), ra1, ra2);
    sys_exit(134);
    spin_after_exit();
}

[[noreturn]] void __cxa_pure_virtual(void) {
    static const char message[] = "[virtua] __cxa_pure_virtual";
    void* const ra1 = __builtin_frame_address(1) ? __builtin_return_address(1) : nullptr;
    void* const ra2 = __builtin_frame_address(2) ? __builtin_return_address(2) : nullptr;
    write_abort_report(message, sizeof(message) - 1, __builtin_return_address(0), ra1, ra2);
    sys_exit(134);
    spin_after_exit();
}

int __cxa_thread_atexit(void (*)(void*), void*, void*) {
    return 0;
}

int __cxa_thread_atexit_impl(void (*)(void*), void*, void*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    if (!attr) return EINVAL;
    attr->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_mutexattr_destroy(pthread_mutexattr_t*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
    if (!attr) return EINVAL;
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE && type != PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    attr->type = type;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type) {
    if (!attr || !type) return EINVAL;
    *type = attr->type;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    if (!mutex) return EINVAL;
    mutex->initialized = 1;
    mutex->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
    mutex->locked = 0;
    mutex->recursion = 0;
    mutex->owner = 0;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_mutex_destroy(pthread_mutex_t*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL pthread_t pthread_self(void) {
    const uintptr_t thread = sys_thread_self();
    return thread == 0 ? 1 : thread;
}

VIRTUA_WEAK_SYMBOL int pthread_equal(pthread_t lhs, pthread_t rhs) {
    return lhs == rhs;
}

VIRTUA_WEAK_SYMBOL int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    ensure_mutex(mutex);
    const pthread_t self = pthread_self();
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&mutex->locked, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            mutex->owner = self;
            mutex->recursion = 1;
            return 0;
        }
        if (mutex->owner == self) {
            if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
                ++mutex->recursion;
                return 0;
            }
            if (mutex->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        }
        sched_yield();
    }
}

VIRTUA_WEAK_SYMBOL int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    ensure_mutex(mutex);
    const pthread_t self = pthread_self();
    int expected = 0;
    if (__atomic_compare_exchange_n(&mutex->locked, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        mutex->owner = self;
        mutex->recursion = 1;
        return 0;
    }
    if (mutex->owner == self && mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        ++mutex->recursion;
        return 0;
    }
    return EBUSY;
}

VIRTUA_WEAK_SYMBOL int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    ensure_mutex(mutex);
    if (!mutex->locked) return EPERM;
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->recursion > 1) {
        --mutex->recursion;
        return 0;
    }
    mutex->recursion = 0;
    mutex->owner = 0;
    __atomic_store_n(&mutex->locked, 0, __ATOMIC_RELEASE);
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t*) {
    if (!cond) return EINVAL;
    cond->initialized = 1;
    cond->sequence = 0;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_cond_destroy(pthread_cond_t*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_cond_signal(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    __atomic_add_fetch(&cond->sequence, 1U, __ATOMIC_RELEASE);
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_cond_broadcast(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    __atomic_add_fetch(&cond->sequence, 1U, __ATOMIC_RELEASE);
    return 0;
}

VIRTUA_WEAK_SYMBOL int sched_yield(void) {
    if (sys_thread_yield() == 0) return 0;
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__arm__) || defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (!cond || !mutex) return EINVAL;
    const unsigned int sequence = __atomic_load_n(&cond->sequence, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(mutex);
    while (__atomic_load_n(&cond->sequence, __ATOMIC_ACQUIRE) == sequence) {
        sleep_or_yield_briefly();
    }
    pthread_mutex_lock(mutex);
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    if (!cond || !mutex) return EINVAL;
    const unsigned int sequence = __atomic_load_n(&cond->sequence, __ATOMIC_ACQUIRE);
    // abstime is a wall-clock instant; the loop below polls the kernel clock.
    // Convert once, here, rather than comparing the two domains directly.
    const bool timed = abstime != nullptr;
    const uint64_t deadline_us = timed ? realtime_deadline_to_kernel_us(timespec_to_us(abstime)) : 0;
    pthread_mutex_unlock(mutex);
    for (;;) {
        if (__atomic_load_n(&cond->sequence, __ATOMIC_ACQUIRE) != sequence) {
            pthread_mutex_lock(mutex);
            return 0;
        }
        if (timed && kernel_monotonic_microseconds() >= deadline_us) break;
        sleep_or_yield_briefly(deadline_us);
    }
    pthread_mutex_lock(mutex);
    return ETIMEDOUT;
}

VIRTUA_WEAK_SYMBOL int pthread_cond_clockwait(pthread_cond_t* cond,
                                              pthread_mutex_t* mutex,
                                              clockid_t,
                                              const struct timespec* abstime) {
    return pthread_cond_timedwait(cond, mutex, abstime);
}

VIRTUA_WEAK_SYMBOL int pthread_condattr_init(pthread_condattr_t* attr) {
    if (!attr) return EINVAL;
    attr->clock_id = CLOCK_REALTIME;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_condattr_destroy(pthread_condattr_t*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock_id) {
    if (!attr) return EINVAL;
    attr->clock_id = clock_id;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_condattr_getclock(const pthread_condattr_t* attr, clockid_t* clock_id) {
    if (!attr || !clock_id) return EINVAL;
    *clock_id = attr->clock_id;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_rwlock_init(pthread_rwlock_t* lock, const void*) {
    if (!lock) return EINVAL;
    return pthread_mutex_init(&lock->mutex, nullptr);
}

VIRTUA_WEAK_SYMBOL int pthread_rwlock_destroy(pthread_rwlock_t* lock) {
    if (!lock) return EINVAL;
    return pthread_mutex_destroy(&lock->mutex);
}

VIRTUA_WEAK_SYMBOL int pthread_rwlock_rdlock(pthread_rwlock_t* lock) {
    if (!lock) return EINVAL;
    return pthread_mutex_lock(&lock->mutex);
}

VIRTUA_WEAK_SYMBOL int pthread_rwlock_wrlock(pthread_rwlock_t* lock) {
    if (!lock) return EINVAL;
    return pthread_mutex_lock(&lock->mutex);
}

VIRTUA_WEAK_SYMBOL int pthread_rwlock_unlock(pthread_rwlock_t* lock) {
    if (!lock) return EINVAL;
    return pthread_mutex_unlock(&lock->mutex);
}

VIRTUA_WEAK_SYMBOL int pthread_once(pthread_once_t* once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    int expected = 0;
    if (__atomic_compare_exchange_n(once_control, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init_routine();
    }
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 0) sched_yield();
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_attr_init(pthread_attr_t* attr) {
    if (!attr) return EINVAL;
    attr->stack_size = 0;
    attr->detach_state = PTHREAD_CREATE_JOINABLE;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_attr_destroy(pthread_attr_t*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_attr_setdetachstate(pthread_attr_t* attr, int detach_state) {
    if (!attr) return EINVAL;
    if (detach_state != PTHREAD_CREATE_JOINABLE && detach_state != PTHREAD_CREATE_DETACHED) return EINVAL;
    attr->detach_state = detach_state;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detach_state) {
    if (!attr || !detach_state) return EINVAL;
    *detach_state = attr->detach_state;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stack_size) {
    if (!attr) return EINVAL;
    attr->stack_size = stack_size;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* stack_size) {
    if (!attr || !stack_size) return EINVAL;
    *stack_size = attr->stack_size;
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_create(pthread_t* thread,
                                      const pthread_attr_t* attr,
                                      void* (*start_routine)(void*),
                                      void* arg) {
    if (!thread || !start_routine) return EINVAL;
    uintptr_t created = 0;
    long rc = sys_thread_create(&created,
                                reinterpret_cast<uintptr_t>(start_routine),
                                reinterpret_cast<uintptr_t>(arg),
                                attr ? attr->stack_size : 0);
    if (rc < 0) return static_cast<int>(-rc);
    if (rc != 0) return static_cast<int>(rc);
    *thread = created;
    if (attr && attr->detach_state == PTHREAD_CREATE_DETACHED) {
        pthread_detach(created);
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL int pthread_join(pthread_t thread, void** retval) {
    long rc = sys_thread_join(thread, retval);
    return rc < 0 ? static_cast<int>(-rc) : static_cast<int>(rc);
}

VIRTUA_WEAK_SYMBOL int pthread_detach(pthread_t thread) {
    long rc = sys_thread_detach(thread);
    return rc < 0 ? static_cast<int>(-rc) : static_cast<int>(rc);
}

VIRTUA_WEAK_SYMBOL void pthread_exit(void* retval) {
    (void)retval;
    sys_exit(0);
    spin_after_exit();
}

VIRTUA_WEAK_SYMBOL int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    if (!key) return EINVAL;
    for (unsigned int i = 0; i < kMaxTlsKeys; ++i) {
        if (!g_tls[i].used) {
            g_tls[i].used = true;
            g_tls[i].destructor = destructor;
            memset(g_tls[i].values, 0, sizeof(g_tls[i].values));
            *key = i;
            fprintf(stderr, "virtua-tls: create key=%u thread=%llu slot=%u\n", i,
                    static_cast<unsigned long long>(sys_thread_self()), current_thread_slot());
            return 0;
        }
    }
    return EAGAIN;
}

VIRTUA_WEAK_SYMBOL int pthread_key_delete(pthread_key_t key) {
    if (key >= kMaxTlsKeys || !g_tls[key].used) return EINVAL;
    g_tls[key] = {};
    return 0;
}

VIRTUA_WEAK_SYMBOL void* pthread_getspecific(pthread_key_t key) {
    if (key >= kMaxTlsKeys || !g_tls[key].used) return nullptr;
    const unsigned int slot = current_thread_slot();
    void* value = g_tls[key].values[slot];
    if (!value && key == 0) {
        fprintf(stderr, "virtua-tls: null get key=%u thread=%llu slot=%u\n", key,
                static_cast<unsigned long long>(sys_thread_self()), slot);
    }
    return value;
}

VIRTUA_WEAK_SYMBOL int pthread_setspecific(pthread_key_t key, const void* value) {
    if (key >= kMaxTlsKeys || !g_tls[key].used) return EINVAL;
    const unsigned int slot = current_thread_slot();
    g_tls[key].values[slot] = const_cast<void*>(value);
    fprintf(stderr, "virtua-tls: set key=%u thread=%llu slot=%u value=%p\n", key,
            static_cast<unsigned long long>(sys_thread_self()), slot, value);
    return 0;
}

__attribute__((visibility("hidden"))) [[noreturn]] void __llvm_libc_exit(int status) {
    sys_exit(status);
    spin_after_exit();
}

__attribute__((visibility("hidden"))) struct __llvm_libc_stdio_cookie
    __llvm_libc_stdin_cookie  = {0};
__attribute__((visibility("hidden"))) struct __llvm_libc_stdio_cookie
    __llvm_libc_stdout_cookie = {1};
__attribute__((visibility("hidden"))) struct __llvm_libc_stdio_cookie
    __llvm_libc_stderr_cookie = {2};

__attribute__((visibility("hidden")))
int __llvm_libc_stdio_write(void* /*cookie*/, const char* buf, size_t size) {
    long n = sys_write(1, buf, size);
    return n < 0 ? -1 : (int)n;
}

__attribute__((visibility("hidden")))
int __llvm_libc_stdio_read(void* /*cookie*/, char* buf, size_t size) {
    long n = sys_read(0, buf, size);
    return n < 0 ? -1 : (int)n;
}

#if defined(__riscv) || defined(__arm__)
__attribute__((visibility("hidden")))
int* __llvm_libc_errno(void) {
    static int shared_errno = 0;
    return &shared_errno;
}
#endif

__attribute__((visibility("hidden")))
bool __llvm_libc_timespec_get_utc(struct timespec* ts) {
    return timespec_get_impl(ts, TIME_UTC) == TIME_UTC;
}

int timespec_get(struct timespec* ts, int base) {
    return timespec_get_impl(ts, base);
}

unsigned long long minos_user_monotonic_microseconds(void) {
    return monotonic_microseconds();
}

long minos_user_gettimeofday(void* tv, void* tz) {
    return sys_gettimeofday(tv, tz);
}

const struct in6_addr in6addr_any = {};
const struct in6_addr in6addr_loopback = {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}};

uint16_t htons(uint16_t value) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#else
    return byteswap16(value);
#endif
}

uint16_t ntohs(uint16_t value) {
    return htons(value);
}

uint32_t htonl(uint32_t value) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#else
    return byteswap32(value);
#endif
}

uint32_t ntohl(uint32_t value) {
    return htonl(value);
}

int inet_pton(int af, const char* src, void* dst) {
    if (!src || !dst) {
        errno = EINVAL;
        return -1;
    }
    if (af == AF_INET) {
        uint8_t bytes[4] {};
        if (!parse_ipv4_address(src, bytes)) return 0;
        memcpy(dst, bytes, sizeof(bytes));
        return 1;
    }
    if (af == AF_INET6) {
        uint8_t bytes[16] {};
        if (!parse_ipv6_loopback_or_any(src, bytes)) return 0;
        memcpy(dst, bytes, sizeof(bytes));
        return 1;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    if (!src || !dst) {
        errno = EINVAL;
        return nullptr;
    }
    if (af == AF_INET) {
        if (size < INET_ADDRSTRLEN) {
            errno = ENOSPC;
            return nullptr;
        }
        const auto* bytes = static_cast<const uint8_t*>(src);
        snprintf(dst, size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
        return dst;
    }
    if (af == AF_INET6) {
        const auto* bytes = static_cast<const uint8_t*>(src);
        bool is_any = true;
        for (int i = 0; i < 16; ++i) {
            if (bytes[i] != 0) {
                is_any = false;
                break;
            }
        }
        bool is_loopback = true;
        for (int i = 0; i < 15; ++i) {
            if (bytes[i] != 0) {
                is_loopback = false;
                break;
            }
        }
        is_loopback = is_loopback && bytes[15] == 1;
        const char* text = is_loopback ? "::1" : (is_any ? "::" : nullptr);
        if (!text) {
            errno = EAFNOSUPPORT;
            return nullptr;
        }
        if (!write_text(dst, size, text)) {
            errno = ENOSPC;
            return nullptr;
        }
        return dst;
    }
    errno = EAFNOSUPPORT;
    return nullptr;
}

int inet_aton(const char* cp, struct in_addr* inp) {
    if (!inp) return 0;
    return inet_pton(AF_INET, cp, &inp->s_addr) == 1 ? 1 : 0;
}

in_addr_t inet_addr(const char* cp) {
    struct in_addr addr {};
    return inet_aton(cp, &addr) ? addr.s_addr : INADDR_NONE;
}

char* inet_ntoa(struct in_addr in) {
    static char buffer[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, &in.s_addr, buffer, sizeof(buffer)) ? buffer : nullptr;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    if (!res) return EAI_FAIL;
    *res = nullptr;

    const int family = hints ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return EAI_FAMILY;

    uint16_t port = 0;
    if (!parse_service_port(service, port)) return EAI_SERVICE;

    uint8_t ipv4[4] {};
    uint8_t ipv6[16] {};
    bool use_ipv6 = false;
    bool resolved = false;

    if (!node || !*node) {
        resolved = true;
        if (family == AF_INET6) {
            use_ipv6 = true;
            memset(ipv6, 0, sizeof(ipv6));
            if (!(hints && (hints->ai_flags & AI_PASSIVE))) ipv6[15] = 1;
        } else {
            uint32_t host_addr = (hints && (hints->ai_flags & AI_PASSIVE)) ? INADDR_ANY : INADDR_LOOPBACK;
            const uint32_t net_addr = htonl(host_addr);
            memcpy(ipv4, &net_addr, sizeof(ipv4));
        }
    } else if ((family == AF_UNSPEC || family == AF_INET) && parse_ipv4_address(node, ipv4)) {
        resolved = true;
    } else if ((family == AF_UNSPEC || family == AF_INET6) && parse_ipv6_loopback_or_any(node, ipv6)) {
        resolved = true;
        use_ipv6 = true;
    } else if (strcmp(node, "localhost") == 0) {
        resolved = true;
        if (family == AF_INET6) {
            use_ipv6 = true;
            memset(ipv6, 0, sizeof(ipv6));
            ipv6[15] = 1;
        } else {
            const uint32_t net_addr = htonl(INADDR_LOOPBACK);
            memcpy(ipv4, &net_addr, sizeof(ipv4));
        }
    }

    if (!resolved) return EAI_NONAME;
    if (hints && (hints->ai_flags & AI_NUMERICHOST) && node && strcmp(node, "localhost") == 0) return EAI_NONAME;

    auto* item = static_cast<VirtuaAddrinfoNode*>(calloc(1, sizeof(VirtuaAddrinfoNode)));
    if (!item) return EAI_MEMORY;
    item->info.ai_flags = hints ? hints->ai_flags : 0;
    item->info.ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    item->info.ai_protocol = hints && hints->ai_protocol ? hints->ai_protocol
                                                          : (item->info.ai_socktype == SOCK_DGRAM ? IPPROTO_UDP
                                                                                                  : IPPROTO_TCP);
    item->info.ai_addr = reinterpret_cast<struct sockaddr*>(&item->storage);
    if (use_ipv6) {
        auto* addr = reinterpret_cast<struct sockaddr_in6*>(&item->storage);
        addr->sin6_family = AF_INET6;
        addr->sin6_port = htons(port);
        memcpy(&addr->sin6_addr, ipv6, sizeof(ipv6));
        item->info.ai_family = AF_INET6;
        item->info.ai_addrlen = sizeof(*addr);
    } else {
        auto* addr = reinterpret_cast<struct sockaddr_in*>(&item->storage);
        addr->sin_family = AF_INET;
        addr->sin_port = htons(port);
        memcpy(&addr->sin_addr.s_addr, ipv4, sizeof(ipv4));
        item->info.ai_family = AF_INET;
        item->info.ai_addrlen = sizeof(*addr);
    }
    if (hints && (hints->ai_flags & AI_CANONNAME) && node && *node) {
        snprintf(item->canonname, sizeof(item->canonname), "%s", node);
        item->info.ai_canonname = item->canonname;
    }
    *res = &item->info;
    return 0;
}

void freeaddrinfo(struct addrinfo* res) {
    while (res) {
        struct addrinfo* next = res->ai_next;
        free(res);
        res = next;
    }
}

const char* gai_strerror(int ecode) {
    switch (ecode) {
    case 0: return "success";
    case EAI_BADFLAGS: return "bad flags";
    case EAI_NONAME: return "name or service not known";
    case EAI_AGAIN: return "temporary failure";
    case EAI_FAIL: return "non-recoverable failure";
    case EAI_FAMILY: return "unsupported address family";
    case EAI_SOCKTYPE: return "unsupported socket type";
    case EAI_SERVICE: return "unsupported service";
    case EAI_MEMORY: return "out of memory";
    case EAI_SYSTEM: return "system error";
    case EAI_OVERFLOW: return "buffer overflow";
    default: return "unknown error";
    }
}

int getnameinfo(const struct sockaddr* sa,
                socklen_t salen,
                char* host,
                socklen_t hostlen,
                char* serv,
                socklen_t servlen,
                int) {
    if (!sa) return EAI_FAIL;
    uint16_t port = 0;
    if (sa->sa_family == AF_INET && salen >= sizeof(struct sockaddr_in)) {
        const auto* in = reinterpret_cast<const struct sockaddr_in*>(sa);
        port = ntohs(in->sin_port);
        if (host && !inet_ntop(AF_INET, &in->sin_addr.s_addr, host, hostlen)) return EAI_OVERFLOW;
    } else if (sa->sa_family == AF_INET6 && salen >= sizeof(struct sockaddr_in6)) {
        const auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        port = ntohs(in6->sin6_port);
        if (host && !inet_ntop(AF_INET6, &in6->sin6_addr, host, hostlen)) return EAI_OVERFLOW;
    } else {
        return EAI_FAMILY;
    }
    if (serv) {
        char service[NI_MAXSERV];
        snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));
        if (!write_text(serv, servlen, service)) return EAI_OVERFLOW;
    }
    return 0;
}

unsigned if_nametoindex(const char* ifname) {
    return (ifname && (strcmp(ifname, "lo") == 0 || strcmp(ifname, "lo0") == 0)) ? 1U : 0U;
}

char* if_indextoname(unsigned ifindex, char* ifname) {
    if (ifindex != 1 || !ifname) {
        errno = ENXIO;
        return nullptr;
    }
    if (!write_text(ifname, IF_NAMESIZE, "lo")) {
        errno = ENOSPC;
        return nullptr;
    }
    return ifname;
}

int getifaddrs(struct ifaddrs** ifap) {
    if (!ifap) {
        errno = EINVAL;
        return -1;
    }
    *ifap = nullptr;
    auto* entry = static_cast<struct ifaddrs*>(calloc(1, sizeof(struct ifaddrs)));
    auto* addr = static_cast<struct sockaddr_in*>(calloc(1, sizeof(struct sockaddr_in)));
    auto* mask = static_cast<struct sockaddr_in*>(calloc(1, sizeof(struct sockaddr_in)));
    char* name = duplicate_c_string("lo");
    if (!entry || !addr || !mask || !name) {
        free(entry);
        free(addr);
        free(mask);
        free(name);
        errno = ENOMEM;
        return -1;
    }
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    mask->sin_family = AF_INET;
    mask->sin_addr.s_addr = htonl(0xff000000U);
    entry->ifa_name = name;
    entry->ifa_flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
    entry->ifa_addr = reinterpret_cast<struct sockaddr*>(addr);
    entry->ifa_netmask = reinterpret_cast<struct sockaddr*>(mask);
    *ifap = entry;
    return 0;
}

void freeifaddrs(struct ifaddrs* ifa) {
    while (ifa) {
        struct ifaddrs* next = ifa->ifa_next;
        free(ifa->ifa_name);
        free(ifa->ifa_addr);
        free(ifa->ifa_netmask);
        free(ifa);
        ifa = next;
    }
}

int clock_gettime(clockid_t clock_id, struct timespec* tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }

    if (clock_id == kClockRealtime) {
        return fill_timespec_from_epoch_usec(tp, realtime_microseconds()) ? 0 : -1;
    }
    if (clock_id == kClockMonotonic || clock_id == kClockMonotonicRaw) {
        return fill_timespec_from_ticks(tp, read_time_ticks()) ? 0 : -1;
    }

    errno = EINVAL;
    return -1;
}

int clock_getres(clockid_t clock_id, struct timespec* tp) {
    if (clock_id != kClockRealtime && clock_id != kClockMonotonic && clock_id != kClockMonotonicRaw) {
        errno = EINVAL;
        return -1;
    }
    if (tp) {
        tp->tv_sec = 0;
        tp->tv_nsec = 1000;
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    const uint64_t start = monotonic_microseconds();
    const uint64_t duration = timespec_to_us(req);
    const uint64_t deadline = clamped_deadline_us(start, duration);
    if (duration == 0 || sys_sleep_until_us(deadline) == 0) return 0;
    while (monotonic_microseconds() < deadline) {
        sleep_or_yield_until(deadline);
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL long sysconf(int name) {
    switch (name) {
    case _SC_NPROCESSORS_ONLN:
        return 1;
    case _SC_PAGE_SIZE:
        return 4096;
    case _SC_ARG_MAX:
        return 262144;
    case _SC_GETPW_R_SIZE_MAX:
        return 1024;
    default:
        break;
    }
    return 1;
}

time_t time(time_t* timer) {
    const time_t now = static_cast<time_t>(realtime_microseconds() / kMicrosPerSecond);
    if (timer) {
        *timer = now;
    }
    return now;
}

VIRTUA_WEAK_SYMBOL struct tm* gmtime_r(const time_t* timer, struct tm* result) {
    if (!timer || !result) {
        errno = EINVAL;
        return nullptr;
    }
    return fill_utc_tm(*timer, result);
}

VIRTUA_WEAK_SYMBOL struct tm* localtime_r(const time_t* timer, struct tm* result) {
    if (!timer) {
        errno = EINVAL;
        return nullptr;
    }
    return gmtime_r(timer, result);
}

VIRTUA_WEAK_SYMBOL struct tm* gmtime(const time_t* timer) {
    static struct tm shared;
    return gmtime_r(timer, &shared);
}

VIRTUA_WEAK_SYMBOL struct tm* localtime(const time_t* timer) {
    static struct tm shared;
    return localtime_r(timer, &shared);
}

VIRTUA_WEAK_SYMBOL size_t strftime(char* out, size_t out_size, const char* format, const struct tm* timeptr) {
    if (!out || out_size == 0 || !format || !timeptr) return 0;
    char* cursor = out;
    size_t remaining = out_size;
    static const char* const kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (const char* p = format; *p; ++p) {
        if (*p != '%') {
            if (!append_char(cursor, remaining, *p)) return 0;
            continue;
        }
        ++p;
        if (!*p) break;
        switch (*p) {
        case '%':
            if (!append_char(cursor, remaining, '%')) return 0;
            break;
        case 'Y':
            if (!append_decimal(cursor, remaining, timeptr->tm_year + 1900, 4)) return 0;
            break;
        case 'y':
            if (!append_decimal(cursor, remaining, (timeptr->tm_year + 1900) % 100, 2)) return 0;
            break;
        case 'm':
            if (!append_decimal(cursor, remaining, timeptr->tm_mon + 1, 2)) return 0;
            break;
        case 'd':
            if (!append_decimal(cursor, remaining, timeptr->tm_mday, 2)) return 0;
            break;
        case 'H':
            if (!append_decimal(cursor, remaining, timeptr->tm_hour, 2)) return 0;
            break;
        case 'M':
            if (!append_decimal(cursor, remaining, timeptr->tm_min, 2)) return 0;
            break;
        case 'S':
            if (!append_decimal(cursor, remaining, timeptr->tm_sec, 2)) return 0;
            break;
        case 'a':
            if (!append_text(cursor, remaining, kWeekdays[(timeptr->tm_wday >= 0 && timeptr->tm_wday < 7) ? timeptr->tm_wday : 0])) return 0;
            break;
        case 'b':
            if (!append_text(cursor, remaining, kMonths[(timeptr->tm_mon >= 0 && timeptr->tm_mon < 12) ? timeptr->tm_mon : 0])) return 0;
            break;
        case 'F':
            if (!append_decimal(cursor, remaining, timeptr->tm_year + 1900, 4) ||
                !append_char(cursor, remaining, '-') ||
                !append_decimal(cursor, remaining, timeptr->tm_mon + 1, 2) ||
                !append_char(cursor, remaining, '-') ||
                !append_decimal(cursor, remaining, timeptr->tm_mday, 2)) return 0;
            break;
        case 'T':
            if (!append_decimal(cursor, remaining, timeptr->tm_hour, 2) ||
                !append_char(cursor, remaining, ':') ||
                !append_decimal(cursor, remaining, timeptr->tm_min, 2) ||
                !append_char(cursor, remaining, ':') ||
                !append_decimal(cursor, remaining, timeptr->tm_sec, 2)) return 0;
            break;
        default:
            if (!append_char(cursor, remaining, *p)) return 0;
            break;
        }
    }
    *cursor = '\0';
    return static_cast<size_t>(cursor - out);
}

VIRTUA_WEAK_SYMBOL locale_t newlocale(int, const char*, locale_t base) {
    static __locale_t c_locale{};
    return base ? base : &c_locale;
}

VIRTUA_WEAK_SYMBOL locale_t duplocale(locale_t locale) {
    return newlocale(LC_ALL_MASK, "C", locale);
}

VIRTUA_WEAK_SYMBOL void freelocale(locale_t) {
}

VIRTUA_WEAK_SYMBOL locale_t uselocale(locale_t locale) {
    static __locale_t c_locale{};
    return locale ? locale : &c_locale;
}

char* setlocale(int, const char*) {
    static char c_locale_name[] = "C";
    return c_locale_name;
}

struct lconv* localeconv(void) {
    static char decimal_point[] = ".";
    static char empty[]         = "";
    static struct lconv c_lconv = {
        decimal_point,
        empty,
        empty,
        empty,
        empty,
        empty,
        empty,
        empty,
        empty,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        empty,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
        CHAR_MAX,
    };
    return &c_lconv;
}

float strtof_l(const char* s, char** end, locale_t) {
    return strtof(s, end);
}

double strtod_l(const char* s, char** end, locale_t) {
    return strtod(s, end);
}

long double strtold_l(const char* s, char** end, locale_t) {
    return strtold(s, end);
}

int isalnum_l(int ch, locale_t) {
    return isalnum(ch);
}

int isalpha_l(int ch, locale_t) {
    return isalpha(ch);
}

int isblank_l(int ch, locale_t) {
    return isblank(ch);
}

int iscntrl_l(int ch, locale_t) {
    return iscntrl(ch);
}

int isdigit_l(int ch, locale_t) {
    return isdigit(ch);
}

int isgraph_l(int ch, locale_t) {
    return isgraph(ch);
}

int islower_l(int ch, locale_t) {
    return islower(ch);
}

int isprint_l(int ch, locale_t) {
    return isprint(ch);
}

int ispunct_l(int ch, locale_t) {
    return ispunct(ch);
}

int isspace_l(int ch, locale_t) {
    return isspace(ch);
}

int isupper_l(int ch, locale_t) {
    return isupper(ch);
}

int isxdigit_l(int ch, locale_t) {
    return isxdigit(ch);
}

int tolower_l(int ch, locale_t) {
    return tolower(ch);
}

int toupper_l(int ch, locale_t) {
    return toupper(ch);
}

int strcoll_l(const char* left, const char* right, locale_t) {
    return strcmp(left ? left : "", right ? right : "");
}

size_t strxfrm_l(char* out, const char* in, size_t out_size, locale_t) {
    if (!in) {
        if (out && out_size) out[0] = '\0';
        return 0;
    }
    const size_t len = strlen(in);
    if (out && out_size) {
        const size_t n = len < out_size - 1 ? len : out_size - 1;
        memcpy(out, in, n);
        out[n] = '\0';
    }
    return len;
}

VIRTUA_WEAK_SYMBOL size_t strftime_l(char* out, size_t out_size, const char* format, const struct tm* timeptr, locale_t) {
    return strftime(out, out_size, format, timeptr);
}

VIRTUA_WEAK_SYMBOL char* getenv(const char*) {
    return nullptr;
}

VIRTUA_WEAK_SYMBOL int setenv(const char*, const char*, int) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int unsetenv(const char*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int putenv(char*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int isatty(int fd) {
    return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
}

VIRTUA_WEAK_SYMBOL int fcntl(int, int, ...) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int access(const char* path, int) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    const long rc = sys_stat(path, &st);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL char* getcwd(char* buf, size_t size) {
    char* target = buf;
    size_t target_size = size;
    if (!target) {
        target_size = size != 0 ? size : static_cast<size_t>(PATH_MAX);
        if (target_size == 0) target_size = 4096;
        target = static_cast<char*>(malloc(target_size));
        if (!target) {
            errno = ENOMEM;
            return nullptr;
        }
    } else if (target_size == 0) {
        errno = EINVAL;
        return nullptr;
    }

    const long rc = sys_getcwd(target, target_size);
    if (rc < 0) {
        if (!buf) free(target);
        errno = static_cast<int>(-rc);
        return nullptr;
    }
    return target;
}

VIRTUA_WEAK_SYMBOL int chdir(const char* path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    const long rc = sys_chdir(path);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL char* strdup(const char* text) {
    if (!text) {
        errno = EINVAL;
        return nullptr;
    }
    const size_t len = strlen(text) + 1;
    char* copy = static_cast<char*>(malloc(len));
    if (!copy) {
        errno = ENOMEM;
        return nullptr;
    }
    memcpy(copy, text, len);
    return copy;
}

VIRTUA_WEAK_SYMBOL int strcasecmp(const char* left, const char* right) {
    if (left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    while (*left && *right) {
        const int lc = tolower(static_cast<unsigned char>(*left++));
        const int rc = tolower(static_cast<unsigned char>(*right++));
        if (lc != rc) return lc - rc;
    }
    return tolower(static_cast<unsigned char>(*left)) - tolower(static_cast<unsigned char>(*right));
}

VIRTUA_WEAK_SYMBOL int strncasecmp(const char* left, const char* right, size_t count) {
    if (count == 0 || left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    for (size_t i = 0; i < count; ++i) {
        const int lc = tolower(static_cast<unsigned char>(left[i]));
        const int rc = tolower(static_cast<unsigned char>(right[i]));
        if (lc != rc || lc == 0 || rc == 0) return lc - rc;
    }
    return 0;
}

static wchar_t virtua_towlower_ascii(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
}

VIRTUA_WEAK_SYMBOL int wcscasecmp(const wchar_t* left, const wchar_t* right) {
    if (left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    while (*left && *right) {
        const wchar_t lc = virtua_towlower_ascii(*left++);
        const wchar_t rc = virtua_towlower_ascii(*right++);
        if (lc != rc) return (lc < rc) ? -1 : 1;
    }
    const wchar_t lc = virtua_towlower_ascii(*left);
    const wchar_t rc = virtua_towlower_ascii(*right);
    if (lc == rc) return 0;
    return (lc < rc) ? -1 : 1;
}

VIRTUA_WEAK_SYMBOL int wcsncasecmp(const wchar_t* left, const wchar_t* right, size_t count) {
    if (count == 0 || left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    for (size_t i = 0; i < count; ++i) {
        const wchar_t lc = virtua_towlower_ascii(left[i]);
        const wchar_t rc = virtua_towlower_ascii(right[i]);
        if (lc != rc) return (lc < rc) ? -1 : 1;
        if (lc == 0) return 0;
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL int remove(const char* path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    long rc = sys_unlink(path);
    if (rc >= 0) return 0;
    errno = static_cast<int>(-rc);
    return -1;
}

VIRTUA_WEAK_SYMBOL int rmdir(const char* path) {
    return remove(path);
}

VIRTUA_WEAK_SYMBOL int rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) {
        errno = EINVAL;
        return -1;
    }
    long rc = sys_rename(oldpath, newpath);
    if (rc >= 0) return 0;
    errno = static_cast<int>(-rc);
    return -1;
}

VIRTUA_WEAK_SYMBOL char* strerror(int errnum) {
    static char buffer[48];
    snprintf(buffer, sizeof(buffer), "error %d", errnum);
    return buffer;
}

VIRTUA_WEAK_SYMBOL char* strerror_r(int errnum, char* buffer, size_t size) {
    if (!buffer || size == 0) return nullptr;
    const char* prefix = "error ";
    size_t pos = 0;
    while (prefix[pos] && pos + 1 < size) {
        buffer[pos] = prefix[pos];
        ++pos;
    }
    char digits[16];
    int dpos = 0;
    unsigned value = errnum < 0 ? static_cast<unsigned>(-errnum) : static_cast<unsigned>(errnum);
    do {
        digits[dpos++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value && dpos < static_cast<int>(sizeof(digits)));
    if (errnum < 0 && pos + 1 < size) buffer[pos++] = '-';
    while (dpos > 0 && pos + 1 < size) buffer[pos++] = digits[--dpos];
    buffer[pos] = '\0';
    return buffer;
}

VIRTUA_WEAK_SYMBOL int snprintf(char* out, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vsnprintf(out, size, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL int minos_snprintf(char* out, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vsnprintf(out, size, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL int sprintf(char* out, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vsnprintf(out, static_cast<size_t>(-1), format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL int vfprintf(FILE* stream, const char* format, va_list args) {
    char buffer[1024];
    const int rc = vsnprintf(buffer, sizeof(buffer), format ? format : "", args);
    if (rc < 0) return rc;
    const size_t n = static_cast<size_t>(rc) < sizeof(buffer) ? static_cast<size_t>(rc) : sizeof(buffer) - 1;
    const int fd = stream == stderr ? 2 : 1;
    return sys_write(fd, buffer, n) < 0 ? -1 : rc;
}

VIRTUA_WEAK_SYMBOL int fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vfprintf(stream, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vfprintf(stdout, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL int minos_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vfprintf(stdout, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!ptr || size == 0 || nmemb == 0) return 0;
    if (nmemb > SIZE_MAX / size || size * nmemb > 64u * 1024u * 1024u) {
        char diagnostic[256];
        const int diagnostic_size = snprintf(
            diagnostic, sizeof(diagnostic),
            "virtua-fwrite: rejected ptr=%p size=%llu nmemb=%llu stream=%p caller=%p\n", ptr,
            static_cast<unsigned long long>(size), static_cast<unsigned long long>(nmemb), stream,
            __builtin_return_address(0));
        if (diagnostic_size > 0) {
            sys_write(2, diagnostic, static_cast<size_t>(diagnostic_size));
        }
        errno = EOVERFLOW;
        return 0;
    }
    const size_t bytes = size * nmemb;
    const int fd = stream_fd(stream);
    if (fd < 0) {
        errno = EBADF;
        return 0;
    }
    const long written = sys_write(fd, ptr, bytes);
    if (written < 0) {
        errno = static_cast<int>(-written);
        return 0;
    }
    return static_cast<size_t>(written) / size;
}

VIRTUA_WEAK_SYMBOL int fputc(int ch, FILE* stream) {
    unsigned char byte = static_cast<unsigned char>(ch);
    return fwrite(&byte, 1, 1, stream) == 1 ? static_cast<int>(byte) : EOF;
}

VIRTUA_WEAK_SYMBOL FILE* fdopen(int fd, const char*) {
    if (fd < 0) {
        errno = EBADF;
        return nullptr;
    }
    auto* stream = static_cast<VirtuaDashFile*>(malloc(sizeof(VirtuaDashFile)));
    if (!stream) {
        errno = ENOMEM;
        return nullptr;
    }
    stream->fd = fd;
    stream->flags = 0;
    stream->buf_cap = 4096;
    stream->buf = static_cast<unsigned char*>(malloc(stream->buf_cap));
    if (!stream->buf) {
        free(stream);
        errno = ENOMEM;
        return nullptr;
    }
    stream->buf_len = 0;
    stream->buf_pos = 0;
    stream->eof = 0;
    stream->err = 0;
    return reinterpret_cast<FILE*>(stream);
}

VIRTUA_WEAK_SYMBOL int fgetc(FILE* stream) {
    unsigned char byte = 0;
    const int fd = stream_fd(stream);
    if (fd < 0) {
        errno = EBADF;
        return EOF;
    }
    const long rc = sys_read(fd, &byte, 1);
    if (rc != 1) {
        if (rc < 0) errno = static_cast<int>(-rc);
        if (stream && stream != stdin && stream != stdout && stream != stderr) {
            auto* virtua = reinterpret_cast<VirtuaDashFile*>(stream);
            if (rc < 0) {
                virtua->err = 1;
            } else {
                virtua->eof = 1;
            }
        }
        return EOF;
    }
    return static_cast<int>(byte);
}

VIRTUA_WEAK_SYMBOL int getc(FILE* stream) {
    return fgetc(stream);
}

VIRTUA_WEAK_SYMBOL int getchar(void) {
    return fgetc(stdin);
}

VIRTUA_WEAK_SYMBOL char* fgets(char* out, int size, FILE* stream) {
    if (!out || size <= 0) {
        errno = EINVAL;
        return nullptr;
    }
    int written = 0;
    while (written + 1 < size) {
        const int ch = fgetc(stream);
        if (ch == EOF) break;
        out[written++] = static_cast<char>(ch);
        if (ch == '\n') break;
    }
    if (written == 0) {
        return nullptr;
    }
    out[written] = '\0';
    return out;
}

VIRTUA_WEAK_SYMBOL int fflush(FILE*) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int fseek(FILE* stream, long offset, int whence) {
    const int fd = stream_fd(stream);
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    const long rc = sys_lseek(fd, offset, whence);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL long ftell(FILE* stream) {
    const int fd = stream_fd(stream);
    if (fd < 0) {
        errno = EBADF;
        return -1L;
    }
    const long rc = sys_lseek(fd, 0, SEEK_CUR);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1L;
    }
    return rc;
}

VIRTUA_WEAK_SYMBOL int fseeko(FILE* stream, off_t offset, int whence) {
    return fseek(stream, static_cast<long>(offset), whence);
}

VIRTUA_WEAK_SYMBOL off64_t lseek64(int fd, off64_t offset, int whence) {
    const long rc = sys_lseek(fd, static_cast<long>(offset), whence);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return static_cast<off64_t>(rc);
}

VIRTUA_WEAK_SYMBOL off_t ftello(FILE* stream) {
    return static_cast<off_t>(ftell(stream));
}

VIRTUA_WEAK_SYMBOL void setbuf(FILE*, char*) {
}

VIRTUA_WEAK_SYMBOL int setvbuf(FILE*, char*, int, size_t) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int fileno(FILE* stream) {
    const int fd = stream_fd(stream);
    if (fd < 0) errno = EBADF;
    return fd;
}

VIRTUA_WEAK_SYMBOL void clearerr(FILE* stream) {
    if (!stream || stream == stdin || stream == stdout || stream == stderr) return;
    auto* virtua = reinterpret_cast<VirtuaDashFile*>(stream);
    virtua->eof = 0;
    virtua->err = 0;
}

VIRTUA_WEAK_SYMBOL int feof(FILE* stream) {
    if (!stream || stream == stdin || stream == stdout || stream == stderr) return 0;
    return reinterpret_cast<VirtuaDashFile*>(stream)->eof;
}

VIRTUA_WEAK_SYMBOL int ferror(FILE* stream) {
    if (!stream || stream == stdin || stream == stdout || stream == stderr) return 0;
    return reinterpret_cast<VirtuaDashFile*>(stream)->err;
}

VIRTUA_WEAK_SYMBOL void perror(const char* text) {
    if (text && *text) {
        fprintf(stderr, "%s: %s\n", text, strerror(errno));
    } else {
        fprintf(stderr, "%s\n", strerror(errno));
    }
}

VIRTUA_WEAK_SYMBOL int vsscanf(const char* input, const char* format, va_list args) {
    if (!input || !format) {
        errno = EINVAL;
        return EOF;
    }
    const char* in = input;
    int assigned = 0;

    for (const char* f = format; *f; ++f) {
        if (is_scan_space(*f)) {
            while (is_scan_space(f[1])) ++f;
            in = skip_scan_space(in);
            continue;
        }
        if (*f != '%') {
            if (*in != *f) break;
            ++in;
            continue;
        }

        ++f;
        if (*f == '%') {
            if (*in != '%') break;
            ++in;
            continue;
        }

        bool suppress = false;
        if (*f == '*') {
            suppress = true;
            ++f;
        }
        while (*f >= '0' && *f <= '9') ++f;
        enum Length { kNone, kShort, kLong, kLongLong, kLongDouble };
        Length length = kNone;
        if (*f == 'h') {
            length = kShort;
            ++f;
            if (*f == 'h') ++f;
        } else if (*f == 'l') {
            length = kLong;
            ++f;
            if (*f == 'l') {
                length = kLongLong;
                ++f;
            }
        } else if (*f == 'L') {
            length = kLongDouble;
            ++f;
        }

        switch (*f) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            bool parsed = false;
            const int base = (*f == 'x' || *f == 'X') ? 16 : (*f == 'o' ? 8 : (*f == 'i' ? 0 : 10));
            const long long value = parse_scan_integer(in, base, &parsed);
            if (!parsed) return assigned ? assigned : EOF;
            if (!suppress) {
                if (length == kLongLong) {
                    *va_arg(args, long long*) = value;
                } else if (length == kLong) {
                    *va_arg(args, long*) = static_cast<long>(value);
                } else if (length == kShort) {
                    *va_arg(args, short*) = static_cast<short>(value);
                } else {
                    *va_arg(args, int*) = static_cast<int>(value);
                }
                ++assigned;
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            char* end = nullptr;
            const double value = parse_float(in, &end);
            if (end == in) return assigned ? assigned : EOF;
            in = end;
            if (!suppress) {
                if (length == kLongDouble) {
                    *va_arg(args, long double*) = static_cast<long double>(value);
                } else if (length == kLong) {
                    *va_arg(args, double*) = value;
                } else {
                    *va_arg(args, float*) = static_cast<float>(value);
                }
                ++assigned;
            }
            break;
        }
        case 's': {
            in = skip_scan_space(in);
            char* out = suppress ? nullptr : va_arg(args, char*);
            size_t copied = 0;
            while (*in && !is_scan_space(*in)) {
                if (out) out[copied] = *in;
                ++copied;
                ++in;
            }
            if (copied == 0) return assigned ? assigned : EOF;
            if (out) {
                out[copied] = '\0';
                ++assigned;
            }
            break;
        }
        case 'c': {
            if (!*in) return assigned ? assigned : EOF;
            if (!suppress) {
                *va_arg(args, char*) = *in;
                ++assigned;
            }
            ++in;
            break;
        }
        case 'n':
            if (!suppress) {
                *va_arg(args, int*) = static_cast<int>(in - input);
            }
            break;
        default:
            return assigned;
        }
    }
    return assigned;
}

VIRTUA_WEAK_SYMBOL int sscanf(const char* input, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vsscanf(input, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL FILE* minos_fopen(const char* path, const char* mode) {
    return fopen(path, mode);
}

VIRTUA_WEAK_SYMBOL int minos_fclose(FILE* stream) {
    return fclose(stream);
}

VIRTUA_WEAK_SYMBOL int minos_fseek(FILE* stream, long offset, int whence) {
    return fseek(stream, offset, whence);
}

VIRTUA_WEAK_SYMBOL long minos_ftell(FILE* stream) {
    return ftell(stream);
}

VIRTUA_WEAK_SYMBOL void minos_rewind(FILE* stream) {
    rewind(stream);
}

VIRTUA_WEAK_SYMBOL int minos_fflush(FILE* stream) {
    return fflush(stream);
}

VIRTUA_WEAK_SYMBOL size_t minos_fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fread(ptr, size, nmemb, stream);
}

VIRTUA_WEAK_SYMBOL size_t minos_fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr, size, nmemb, stream);
}

VIRTUA_WEAK_SYMBOL int minos_ferror(FILE* stream) {
    return ferror(stream);
}

VIRTUA_WEAK_SYMBOL int minos_fgetc(FILE* stream) {
    return fgetc(stream);
}

VIRTUA_WEAK_SYMBOL int minos_getc(FILE* stream) {
    return getc(stream);
}

VIRTUA_WEAK_SYMBOL int minos_fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vfprintf(stream, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL int minos_sprintf(char* out, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vsnprintf(out, static_cast<size_t>(-1), format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL int minos_sscanf(const char* input, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vsscanf(input, format, args);
    va_end(args);
    return rc;
}

VIRTUA_WEAK_SYMBOL double strtod(const char* s, char** end) {
    return parse_float(s, end);
}

VIRTUA_WEAK_SYMBOL float strtof(const char* s, char** end) {
    return static_cast<float>(parse_float(s, end));
}

VIRTUA_WEAK_SYMBOL long double strtold(const char* s, char** end) {
    return static_cast<long double>(parse_float(s, end));
}

VIRTUA_WEAK_SYMBOL size_t wcslen(const wchar_t* text) {
    size_t len = 0;
    if (!text) return 0;
    while (text[len]) ++len;
    return len;
}

VIRTUA_WEAK_SYMBOL int wcscmp(const wchar_t* left, const wchar_t* right) {
    if (left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return (*left < *right) ? -1 : (*left > *right ? 1 : 0);
}

VIRTUA_WEAK_SYMBOL int wcsncmp(const wchar_t* left, const wchar_t* right, size_t count) {
    if (left == right || count == 0) return 0;
    if (!left) return -1;
    if (!right) return 1;
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
        if (left[i] == 0) return 0;
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL wchar_t* wcscpy(wchar_t* out, const wchar_t* in) {
    wchar_t* start = out;
    if (!out || !in) return out;
    do {
        *out++ = *in;
    } while (*in++);
    return start;
}

VIRTUA_WEAK_SYMBOL wchar_t* wcsncpy(wchar_t* out, const wchar_t* in, size_t count) {
    if (!out) return out;
    size_t i = 0;
    for (; i < count && in && in[i]; ++i) out[i] = in[i];
    for (; i < count; ++i) out[i] = 0;
    return out;
}

VIRTUA_WEAK_SYMBOL const wchar_t* wcschr(const wchar_t* text, wchar_t ch) {
    if (!text) return nullptr;
    while (*text) {
        if (*text == ch) return text;
        ++text;
    }
    return ch == 0 ? text : nullptr;
}

VIRTUA_WEAK_SYMBOL const wchar_t* wcsrchr(const wchar_t* text, wchar_t ch) {
    if (!text) return nullptr;
    const wchar_t* found = nullptr;
    do {
        if (*text == ch) found = text;
    } while (*text++);
    return found;
}

VIRTUA_WEAK_SYMBOL const wchar_t* wmemchr(const wchar_t* text, wchar_t ch, size_t count) {
    if (!text) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (text[i] == ch) return &text[i];
    }
    return nullptr;
}

VIRTUA_WEAK_SYMBOL int wmemcmp(const wchar_t* left, const wchar_t* right, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    return 0;
}

VIRTUA_WEAK_SYMBOL wchar_t* wmemcpy(wchar_t* out, const wchar_t* in, size_t count) {
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return out;
}

VIRTUA_WEAK_SYMBOL wchar_t* wmemmove(wchar_t* out, const wchar_t* in, size_t count) {
    if (out < in) {
        for (size_t i = 0; i < count; ++i) out[i] = in[i];
    } else if (out > in) {
        for (size_t i = count; i > 0; --i) out[i - 1] = in[i - 1];
    }
    return out;
}

VIRTUA_WEAK_SYMBOL wchar_t* wmemset(wchar_t* out, wchar_t ch, size_t count) {
    for (size_t i = 0; i < count; ++i) out[i] = ch;
    return out;
}

VIRTUA_WEAK_SYMBOL wint_t btowc(int ch) {
    if (ch == EOF) return WEOF;
    const unsigned char c = static_cast<unsigned char>(ch);
    return static_cast<wint_t>(c);
}

VIRTUA_WEAK_SYMBOL int wctob(wint_t ch) {
    return (ch >= 0 && ch < 256) ? static_cast<int>(ch) : EOF;
}

VIRTUA_WEAK_SYMBOL size_t mbrtowc(wchar_t* out, const char* in, size_t in_size, mbstate_t*) {
    if (!in) return 0;
    return decode_ascii_wchar(out, in, in_size);
}

VIRTUA_WEAK_SYMBOL int mbtowc(wchar_t* out, const char* in, size_t in_size) {
    const size_t rc = mbrtowc(out, in, in_size, nullptr);
    if (rc == static_cast<size_t>(-1) || rc == static_cast<size_t>(-2)) return -1;
    return static_cast<int>(rc);
}

VIRTUA_WEAK_SYMBOL size_t mbrlen(const char* in, size_t in_size, mbstate_t* state) {
    return mbrtowc(nullptr, in, in_size, state);
}

VIRTUA_WEAK_SYMBOL size_t wcrtomb(char* out, wchar_t ch, mbstate_t*) {
    return encode_ascii_wchar(out, ch);
}

VIRTUA_WEAK_SYMBOL size_t mbsrtowcs(wchar_t* out, const char** in, size_t out_count, mbstate_t*) {
    if (!in || !*in) return 0;
    const char* src = *in;
    size_t count = 0;
    while (*src) {
        if (out) {
            if (count >= out_count) break;
            out[count] = static_cast<unsigned char>(*src);
        }
        ++count;
        ++src;
    }
    if (out && count < out_count) out[count] = 0;
    *in = *src ? src : nullptr;
    return count;
}

VIRTUA_WEAK_SYMBOL size_t mbsnrtowcs(wchar_t* out, const char** in, size_t in_count, size_t out_count, mbstate_t*) {
    if (!in || !*in) return 0;
    const char* src = *in;
    size_t count = 0;
    size_t used = 0;
    while (used < in_count && src[used]) {
        if (out) {
            if (count >= out_count) break;
            out[count] = static_cast<unsigned char>(src[used]);
        }
        ++count;
        ++used;
    }
    if (out && count < out_count) out[count] = 0;
    *in = (used < in_count && src[used]) ? (src + used) : nullptr;
    return count;
}

VIRTUA_WEAK_SYMBOL size_t wcsnrtombs(char* out, const wchar_t** in, size_t in_count, size_t out_count, mbstate_t*) {
    if (!in || !*in) return 0;
    const wchar_t* src = *in;
    size_t count = 0;
    size_t used = 0;
    while (used < in_count && src[used]) {
        if (out) {
            if (count >= out_count) break;
            out[count] = (src[used] >= 0 && src[used] < 128) ? static_cast<char>(src[used]) : '?';
        }
        ++count;
        ++used;
    }
    if (out && count < out_count) out[count] = '\0';
    *in = (used < in_count && src[used]) ? (src + used) : nullptr;
    return count;
}

VIRTUA_WEAK_SYMBOL int feclearexcept(int) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int fetestexcept(int) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int feraiseexcept(int) {
    return 0;
}

VIRTUA_WEAK_SYMBOL int fegetround(void) {
#ifdef FE_TONEAREST
    return FE_TONEAREST;
#else
    return 0;
#endif
}

VIRTUA_WEAK_SYMBOL int fesetround(int) {
    return 0;
}

VIRTUA_WEAK_SYMBOL double fabs(double x) {
    return x < 0.0 ? -x : x;
}

VIRTUA_WEAK_SYMBOL float fabsf(float x) {
    return x < 0.0f ? -x : x;
}

VIRTUA_WEAK_SYMBOL double ceil(double x) {
    long long truncated = static_cast<long long>(x);
    double value = static_cast<double>(truncated);
    return value < x ? value + 1.0 : value;
}

VIRTUA_WEAK_SYMBOL float ceilf(float x) {
    return static_cast<float>(ceil(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double floor(double x) {
    long long truncated = static_cast<long long>(x);
    double value = static_cast<double>(truncated);
    return value > x ? value - 1.0 : value;
}

VIRTUA_WEAK_SYMBOL float floorf(float x) {
    return static_cast<float>(floor(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double round(double x) {
    return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
}

VIRTUA_WEAK_SYMBOL float roundf(float x) {
    return static_cast<float>(round(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double rint(double x) {
    return round(x);
}

VIRTUA_WEAK_SYMBOL float rintf(float x) {
    return static_cast<float>(rint(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL long lrint(double x) {
    return static_cast<long>(rint(x));
}

VIRTUA_WEAK_SYMBOL long long llrint(double x) {
    return static_cast<long long>(rint(x));
}

VIRTUA_WEAK_SYMBOL long lround(double x) {
    return static_cast<long>(round(x));
}

VIRTUA_WEAK_SYMBOL long long llround(double x) {
    return static_cast<long long>(round(x));
}

VIRTUA_WEAK_SYMBOL double modf(double x, double* intpart) {
    const double whole = x < 0.0 ? ceil(x) : floor(x);
    if (intpart) *intpart = whole;
    return x - whole;
}

VIRTUA_WEAK_SYMBOL float modff(float x, float* intpart) {
    double whole = 0.0;
    const double frac = modf(static_cast<double>(x), &whole);
    if (intpart) *intpart = static_cast<float>(whole);
    return static_cast<float>(frac);
}

static double reduce_angle(double x) {
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr double kPi = 3.14159265358979323846;
    while (x > kPi) x -= kTwoPi;
    while (x < -kPi) x += kTwoPi;
    return x;
}

VIRTUA_WEAK_SYMBOL double sin(double x) {
    x = reduce_angle(x);
    const double x2 = x * x;
    return x * (1.0 - x2 / 6.0 + (x2 * x2) / 120.0 - (x2 * x2 * x2) / 5040.0);
}

VIRTUA_WEAK_SYMBOL double cos(double x) {
    x = reduce_angle(x);
    const double x2 = x * x;
    return 1.0 - x2 / 2.0 + (x2 * x2) / 24.0 - (x2 * x2 * x2) / 720.0;
}

VIRTUA_WEAK_SYMBOL double tan(double x) {
    const double c = cos(x);
    return c == 0.0 ? 0.0 : sin(x) / c;
}

VIRTUA_WEAK_SYMBOL float sinf(float x) {
    return static_cast<float>(sin(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL float cosf(float x) {
    return static_cast<float>(cos(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL float tanf(float x) {
    return static_cast<float>(tan(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double atan(double x) {
    constexpr double kHalfPi = 1.57079632679489661923;
    if (x > 1.0) return kHalfPi - atan(1.0 / x);
    if (x < -1.0) return -kHalfPi - atan(1.0 / x);
    return x / (1.0 + 0.28 * x * x);
}

VIRTUA_WEAK_SYMBOL double atan2(double y, double x) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kHalfPi = 1.57079632679489661923;
    if (x > 0.0) return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + kPi;
    if (x < 0.0) return atan(y / x) - kPi;
    if (y > 0.0) return kHalfPi;
    if (y < 0.0) return -kHalfPi;
    return 0.0;
}

VIRTUA_WEAK_SYMBOL float atan2f(float y, float x) {
    return static_cast<float>(atan2(static_cast<double>(y), static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double asin(double x) {
    if (x >= 1.0) return 1.57079632679489661923;
    if (x <= -1.0) return -1.57079632679489661923;
    return atan2(x, sqrt(1.0 - x * x));
}

VIRTUA_WEAK_SYMBOL float asinf(float x) {
    return static_cast<float>(asin(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double acos(double x) {
    return 1.57079632679489661923 - asin(x);
}

VIRTUA_WEAK_SYMBOL float acosf(float x) {
    return static_cast<float>(acos(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL float atanf(float x) {
    return static_cast<float>(atan(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double log2(double x) {
    return log(x) / 0.69314718055994530942;
}

VIRTUA_WEAK_SYMBOL float log2f(float x) {
    return static_cast<float>(log2(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double log10(double x) {
    return log(x) / 2.30258509299404568402;
}

VIRTUA_WEAK_SYMBOL float logf(float x) {
    return static_cast<float>(log(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL float log10f(float x) {
    return static_cast<float>(log10(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double log1p(double x) {
    return log(1.0 + x);
}

VIRTUA_WEAK_SYMBOL double logb(double x) {
    return floor(log2(fabs(x)));
}

VIRTUA_WEAK_SYMBOL double sinh(double x) {
    const double e = exp(x);
    const double inv = e == 0.0 ? 0.0 : 1.0 / e;
    return (e - inv) * 0.5;
}

VIRTUA_WEAK_SYMBOL double cosh(double x) {
    const double e = exp(x);
    const double inv = e == 0.0 ? 0.0 : 1.0 / e;
    return (e + inv) * 0.5;
}

VIRTUA_WEAK_SYMBOL double tanh(double x) {
    const double c = cosh(x);
    return c == 0.0 ? 0.0 : sinh(x) / c;
}

VIRTUA_WEAK_SYMBOL float sinhf(float x) {
    return static_cast<float>(sinh(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL float coshf(float x) {
    return static_cast<float>(cosh(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL float tanhf(float x) {
    return static_cast<float>(tanh(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL float expf(float x) {
    return static_cast<float>(exp(static_cast<double>(x)));
}

VIRTUA_WEAK_SYMBOL double ldexp(double x, int exponent) {
    double scale = 1.0;
    const int count = exponent < 0 ? -exponent : exponent;
    for (int i = 0; i < count; ++i) {
        scale *= 2.0;
    }
    return exponent < 0 ? (x / scale) : (x * scale);
}

VIRTUA_WEAK_SYMBOL float ldexpf(float x, int exponent) {
    return static_cast<float>(ldexp(static_cast<double>(x), exponent));
}

VIRTUA_WEAK_SYMBOL double erf(double x) {
    const double sign = x < 0.0 ? -1.0 : 1.0;
    if (x < 0.0) x = -x;
    const double t = 1.0 / (1.0 + 0.3275911 * x);
    const double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return sign * y;
}

VIRTUA_WEAK_SYMBOL int vasprintf(char** out, const char* format, va_list args) {
    if (!out || !format) {
        errno = EINVAL;
        return -1;
    }
    char formatted[4096];
    va_list copy;
    va_copy(copy, args);
    const int needed = vsnprintf(formatted, sizeof(formatted), format, copy);
    va_end(copy);
    if (needed < 0) {
        *out = nullptr;
        return needed;
    }
    if (static_cast<size_t>(needed) >= sizeof(formatted)) {
        *out = nullptr;
        errno = EOVERFLOW;
        return -1;
    }
    char* buffer = static_cast<char*>(malloc(static_cast<size_t>(needed) + 1));
    if (!buffer) {
        *out = nullptr;
        errno = ENOMEM;
        return -1;
    }
    memcpy(buffer, formatted, static_cast<size_t>(needed) + 1);
    *out = buffer;
    return needed;
}

VIRTUA_WEAK_SYMBOL int asprintf(char** out, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vasprintf(out, format, args);
    va_end(args);
    return rc;
}

int vswprintf(wchar_t* out, size_t out_size, const wchar_t* format, va_list args) {
    if (!out || out_size == 0 || !format) {
        errno = EINVAL;
        return -1;
    }

    char narrow_format[256];
    size_t fmt_index = 0;
    for (; fmt_index + 1 < sizeof(narrow_format) && format[fmt_index]; ++fmt_index) {
        const wchar_t ch = format[fmt_index];
        narrow_format[fmt_index] = (ch >= 0 && ch < 128) ? static_cast<char>(ch) : '?';
    }
    narrow_format[fmt_index] = '\0';

    char narrow_output[256];
    const int rc = vsnprintf(narrow_output, sizeof(narrow_output), narrow_format, args);
    if (rc < 0) {
        return rc;
    }

    size_t copy_count = static_cast<size_t>(rc);
    if (copy_count >= out_size) {
        copy_count = out_size - 1;
    }
    if (copy_count >= sizeof(narrow_output)) {
        copy_count = sizeof(narrow_output) - 1;
    }
    for (size_t i = 0; i < copy_count; ++i) {
        out[i] = static_cast<unsigned char>(narrow_output[i]);
    }
    out[copy_count] = 0;
    return static_cast<int>(copy_count);
}

int swprintf(wchar_t* out, size_t out_size, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vswprintf(out, out_size, format, args);
    va_end(args);
    return rc;
}

int vfwprintf(FILE*, const wchar_t* format, va_list args) {
    wchar_t wide_buffer[256];
    const int rc = vswprintf(wide_buffer, sizeof(wide_buffer) / sizeof(wide_buffer[0]), format, args);
    if (rc <= 0) {
        return rc;
    }
    char narrow_buffer[256];
    size_t count = static_cast<size_t>(rc);
    if (count >= sizeof(narrow_buffer)) {
        count = sizeof(narrow_buffer) - 1;
    }
    for (size_t i = 0; i < count; ++i) {
        const wchar_t ch = wide_buffer[i];
        narrow_buffer[i] = (ch >= 0 && ch < 128) ? static_cast<char>(ch) : '?';
    }
    sys_write(1, narrow_buffer, count);
    return static_cast<int>(count);
}

int fwprintf(FILE* file, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vfwprintf(file, format, args);
    va_end(args);
    return rc;
}

int vwprintf(const wchar_t* format, va_list args) {
    return vfwprintf(stdout, format, args);
}

int wprintf(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vwprintf(format, args);
    va_end(args);
    return rc;
}

int vfwscanf(FILE*, const wchar_t*, va_list) {
    return EOF;
}

int fwscanf(FILE* file, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vfwscanf(file, format, args);
    va_end(args);
    return rc;
}

int vswscanf(const wchar_t*, const wchar_t*, va_list) {
    return EOF;
}

int swscanf(const wchar_t* input, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vswscanf(input, format, args);
    va_end(args);
    return rc;
}

int vwscanf(const wchar_t* format, va_list args) {
    return vfwscanf(stdin, format, args);
}

int wscanf(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int rc = vwscanf(format, args);
    va_end(args);
    return rc;
}

wint_t fgetwc(FILE*) {
    char ch = 0;
    return sys_read(0, &ch, 1) == 1 ? static_cast<unsigned char>(ch) : WEOF;
}

wchar_t* fgetws(wchar_t* out, int count, FILE* file) {
    if (!out || count <= 0) {
        return nullptr;
    }
    int index = 0;
    while (index + 1 < count) {
        const wint_t ch = fgetwc(file);
        if (ch == WEOF) break;
        out[index++] = static_cast<wchar_t>(ch);
        if (ch == '\n') break;
    }
    out[index] = 0;
    return index > 0 ? out : nullptr;
}

wint_t fputwc(wchar_t ch, FILE*) {
    const char narrow = (ch >= 0 && ch < 128) ? static_cast<char>(ch) : '?';
    return sys_write(1, &narrow, 1) == 1 ? ch : WEOF;
}

int fputws(const wchar_t* text, FILE* file) {
    if (!text) {
        errno = EINVAL;
        return -1;
    }
    int written = 0;
    for (; *text; ++text) {
        if (fputwc(*text, file) == WEOF) {
            return -1;
        }
        ++written;
    }
    return written;
}

int fwide(FILE*, int) {
    return 1;
}

wint_t getwc(FILE* file) {
    return fgetwc(file);
}

wint_t getwchar(void) {
    return fgetwc(stdin);
}

wint_t putwc(wchar_t ch, FILE* file) {
    return fputwc(ch, file);
}

wint_t putwchar(wchar_t ch) {
    return fputwc(ch, stdout);
}

wint_t ungetwc(wint_t, FILE*) {
    return WEOF;
}

size_t wcsftime(wchar_t* out, size_t out_size, const wchar_t* format, const struct tm* timeptr) {
    if (!out || out_size == 0 || !format) {
        return 0;
    }
    char narrow_format[128];
    size_t fmt_index = 0;
    for (; fmt_index + 1 < sizeof(narrow_format) && format[fmt_index]; ++fmt_index) {
        const wchar_t ch = format[fmt_index];
        narrow_format[fmt_index] = (ch >= 0 && ch < 128) ? static_cast<char>(ch) : '?';
    }
    narrow_format[fmt_index] = '\0';
    char narrow_out[128];
    const size_t rc = strftime(narrow_out, sizeof(narrow_out), narrow_format, timeptr);
    if (rc == 0) {
        return 0;
    }
    size_t count = rc;
    if (count >= out_size) {
        count = out_size - 1;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<unsigned char>(narrow_out[i]);
    }
    out[count] = 0;
    return count;
}

int iswalnum(wint_t ch) {
    return (ch >= 0 && ch < 128) ? isalnum(static_cast<int>(ch)) : 0;
}

int iswalpha(wint_t ch) {
    return (ch >= 0 && ch < 128) ? isalpha(static_cast<int>(ch)) : 0;
}

int iswblank(wint_t ch) {
    return ch == ' ' || ch == '\t';
}

int iswcntrl(wint_t ch) {
    return (ch >= 0 && ch < 128) ? iscntrl(static_cast<int>(ch)) : 0;
}

int iswdigit(wint_t ch) {
    return (ch >= 0 && ch < 128) ? isdigit(static_cast<int>(ch)) : 0;
}

int iswgraph(wint_t ch) {
    return (ch >= 0 && ch < 128) ? isgraph(static_cast<int>(ch)) : 0;
}

int iswlower(wint_t ch) {
    return (ch >= 'a' && ch <= 'z');
}

int iswprint(wint_t ch) {
    return (ch >= 0 && ch < 128) ? isprint(static_cast<int>(ch)) : 0;
}

int iswpunct(wint_t ch) {
    return (ch >= 0 && ch < 128) ? ispunct(static_cast<int>(ch)) : 0;
}

int iswspace(wint_t ch) {
    return (ch >= 0 && ch < 128) ? isspace(static_cast<int>(ch)) : 0;
}

int iswupper(wint_t ch) {
    return (ch >= 'A' && ch <= 'Z');
}

int iswxdigit(wint_t ch) {
    return (ch >= 0 && ch < 128) ? isxdigit(static_cast<int>(ch)) : 0;
}

int iswctype(wint_t ch, wctype_t desc) {
    return ascii_iswctype(ch, desc);
}

wctype_t wctype(const char* property) {
    return static_cast<wctype_t>(parse_wctype_name(property));
}

wint_t towlower(wint_t ch) {
    return (ch >= 0 && ch < 128) ? ascii_lower(static_cast<int>(ch)) : ch;
}

wint_t towupper(wint_t ch) {
    return (ch >= 0 && ch < 128) ? ascii_upper(static_cast<int>(ch)) : ch;
}

wctrans_t wctrans(const char* property) {
    if (!property) return 0;
    if (strcmp(property, "tolower") == 0) return 1;
    if (strcmp(property, "toupper") == 0) return 2;
    return 0;
}

wint_t towctrans(wint_t ch, wctrans_t desc) {
    if (desc == 1) return towlower(ch);
    if (desc == 2) return towupper(ch);
    return ch;
}

int iswctype_l(wint_t ch, wctype_t desc, locale_t) {
    return iswctype(ch, desc);
}

int iswspace_l(wint_t ch, locale_t) {
    return iswspace(ch);
}

int iswprint_l(wint_t ch, locale_t) {
    return iswprint(ch);
}

int iswcntrl_l(wint_t ch, locale_t) {
    return iswcntrl(ch);
}

int iswupper_l(wint_t ch, locale_t) {
    return iswupper(ch);
}

int iswlower_l(wint_t ch, locale_t) {
    return iswlower(ch);
}

int iswalpha_l(wint_t ch, locale_t) {
    return iswalpha(ch);
}

int iswblank_l(wint_t ch, locale_t) {
    return iswblank(ch);
}

int iswdigit_l(wint_t ch, locale_t) {
    return iswdigit(ch);
}

int iswpunct_l(wint_t ch, locale_t) {
    return iswpunct(ch);
}

int iswxdigit_l(wint_t ch, locale_t) {
    return iswxdigit(ch);
}

wint_t towupper_l(wint_t ch, locale_t) {
    return towupper(ch);
}

wint_t towlower_l(wint_t ch, locale_t) {
    return towlower(ch);
}

int wcscoll(const wchar_t* left, const wchar_t* right) {
    return wcscmp(left ? left : L"", right ? right : L"");
}

size_t wcsxfrm(wchar_t* out, const wchar_t* in, size_t out_size) {
    if (!in) {
        if (out && out_size) out[0] = 0;
        return 0;
    }
    const size_t len = wcslen(in);
    if (out && out_size) {
        const size_t n = len < out_size - 1 ? len : out_size - 1;
        wmemcpy(out, in, n);
        out[n] = 0;
    }
    return len;
}

int wcscoll_l(const wchar_t* left, const wchar_t* right, locale_t) {
    return wcscoll(left, right);
}

size_t wcsxfrm_l(wchar_t* out, const wchar_t* in, size_t out_size, locale_t) {
    return wcsxfrm(out, in, out_size);
}

unsigned int arc4random(void) {
    static uint64_t state = 0;
    if (state == 0) {
        state = monotonic_microseconds() ^ 0x9E3779B97F4A7C15ULL;
    }
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<unsigned int>(state >> 16);
}

int raise(int) {
    return 0;
}

int sigemptyset(sigset_t* set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t* set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = ~static_cast<sigset_t>(0);
    return 0;
}

int sigaddset(sigset_t* set, int sig) {
    if (!set || sig <= 0 || sig >= static_cast<int>(sizeof(sigset_t) * 8)) {
        errno = EINVAL;
        return -1;
    }
    *set |= (static_cast<sigset_t>(1) << static_cast<unsigned>(sig));
    return 0;
}

int sigprocmask(int, const sigset_t*, sigset_t* oldset) {
    if (oldset) {
        *oldset = 0;
    }
    return 0;
}

int sigaction(int, const struct sigaction*, struct sigaction* oldact) {
    if (oldact) {
        oldact->sa_handler = nullptr;
        oldact->sa_mask = 0;
        oldact->sa_flags = 0;
    }
    return 0;
}

sighandler_t signal(int, sighandler_t handler) {
    return handler;
}

int dup(int fd) {
    const long rc = sys_dup3(fd, fd, 0);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return static_cast<int>(rc);
}

int dup2(int oldfd, int newfd) {
    const long rc = sys_dup3(oldfd, newfd, 0);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return static_cast<int>(rc);
}

int pipe(int fds[2]) {
    const long rc = sys_pipe2(fds, 0);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return static_cast<int>(rc);
}

int pipe2(int fds[2], int flags) {
    const long rc = sys_pipe2(fds, flags);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return static_cast<int>(rc);
}

int gethostname(char* name, size_t len) {
    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    static const char hostname[] = "virtua";
    const size_t copy_count = sizeof(hostname) < len ? sizeof(hostname) : len;
    memcpy(name, hostname, copy_count);
    name[len - 1] = '\0';
    return 0;
}

pid_t getsid(pid_t) {
    errno = ESRCH;
    return -1;
}

pid_t getpid(void) {
    return 1;
}

uid_t getuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

int execv(const char*, char* const[]) {
    errno = ENOSYS;
    return -1;
}

int execl(const char*, const char*, ...) {
    errno = ENOSYS;
    return -1;
}

int execvp(const char*, char* const[]) {
    errno = ENOSYS;
    return -1;
}

int execve(const char*, char* const[], char* const[]) {
    errno = ENOSYS;
    return -1;
}

int posix_spawn(pid_t* pid,
                const char*,
                const posix_spawn_file_actions_t*,
                const posix_spawnattr_t*,
                char* const[],
                char* const[])
{
    if (pid) {
        *pid = -1;
    }
    return ENOSYS;
}

int posix_spawnp(pid_t* pid,
                 const char*,
                 const posix_spawn_file_actions_t*,
                 const posix_spawnattr_t*,
                 char* const[],
                 char* const[])
{
    if (pid) {
        *pid = -1;
    }
    return ENOSYS;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* file_actions)
{
    if (file_actions) {
        *file_actions = posix_spawn_file_actions_t{};
    }
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*)
{
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int)
{
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int, int)
{
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int, const char*, int, mode_t)
{
    return 0;
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t*, const char*)
{
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t* attr)
{
    if (attr) {
        *attr = posix_spawnattr_t{};
    }
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t*)
{
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t*, short)
{
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t*, pid_t)
{
    return 0;
}

int openpty(int* amaster, int* aslave, char*, const struct termios*, const struct winsize*)
{
    if (amaster) {
        *amaster = -1;
    }
    if (aslave) {
        *aslave = -1;
    }
    errno = ENOSYS;
    return -1;
}

int mkstemp(char*)
{
    errno = ENOSYS;
    return -1;
}

pid_t setsid(void) {
    return 1;
}

unsigned int alarm(unsigned int) {
    return 0;
}

int kill(pid_t, int) {
    errno = ESRCH;
    return -1;
}

ssize_t readlink(const char*, char*, size_t) {
    errno = ENOENT;
    return -1;
}

int ftruncate(int, off_t) {
    return 0;
}

int ftruncate64(int fd, off64_t length) {
    return ftruncate(fd, static_cast<off_t>(length));
}

int truncate(const char* path, off_t length) {
    const int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    const int rc = ftruncate(fd, length);
    close(fd);
    return rc;
}

long pathconf(const char*, int name) {
    if (name == _PC_PATH_MAX) return 4096;
    errno = EINVAL;
    return -1;
}

int utimes(const char*, const struct timeval[2]) {
    return 0;
}

int lstat(const char* path, struct stat* buf) {
    return stat(path, buf);
}

int chmod(const char*, mode_t) {
    return 0;
}

int fchmod(int, mode_t) {
    return 0;
}

int fchmodat(int, const char* path, mode_t mode, int) {
    return chmod(path, mode);
}

VIRTUA_WEAK_SYMBOL int openat(int dirfd_value, const char* path, int flags, ...) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    mode_t mode = 0666;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    if (dirfd_value != AT_FDCWD && path[0] != '/') {
        errno = ENOSYS;
        return -1;
    }
    return open(path, flags, mode);
}

int chown(const char*, uid_t, gid_t) {
    return 0;
}

int fchown(int, uid_t, gid_t) {
    return 0;
}

mode_t umask(mode_t) {
    return 0;
}

int symlink(const char*, const char*) {
    errno = ENOSYS;
    return -1;
}

int link(const char*, const char*) {
    errno = ENOSYS;
    return -1;
}

DIR* fdopendir(int) {
    errno = ENOSYS;
    return nullptr;
}

int dirfd(DIR*) {
    errno = ENOSYS;
    return -1;
}

int unlinkat(int dirfd_value, const char* path, int flags) {
    (void)dirfd_value;
    if ((flags & AT_REMOVEDIR) != 0) return rmdir(path);
    return unlink(path);
}

static int virtua_ftw_base_index(const char* path) {
    if (!path) return 0;
    int last = 0;
    for (int i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '/') last = i + 1;
    }
    return last;
}

static char* virtua_ftw_join(const char* dir, const char* name) {
    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    const bool need_slash = dir_len > 0 && dir[dir_len - 1] != '/';
    char* joined = static_cast<char*>(malloc(dir_len + (need_slash ? 1 : 0) + name_len + 1));
    if (!joined) return nullptr;
    memcpy(joined, dir, dir_len);
    size_t pos = dir_len;
    if (need_slash) joined[pos++] = '/';
    memcpy(joined + pos, name, name_len + 1);
    return joined;
}

static int virtua_nftw_visit(const char* path, __ftw_func_t fn, int flags, int level) {
    struct stat st {};
    struct FTW info {};
    info.base = virtua_ftw_base_index(path);
    info.level = level;

    if (stat(path, &st) != 0) return fn(path, &st, FTW_NS, &info);

    if (!S_ISDIR(st.st_mode)) return fn(path, &st, FTW_F, &info);

    const bool depth_first = (flags & FTW_DEPTH) != 0;
    if (!depth_first) {
        const int rc = fn(path, &st, FTW_D, &info);
        if (rc != 0) return rc;
    }

    DIR* dir = opendir(path);
    if (!dir) return fn(path, &st, FTW_DNR, &info);

    int result = 0;
    while (struct dirent* entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char* child = virtua_ftw_join(path, entry->d_name);
        if (!child) {
            result = -1;
            errno = ENOMEM;
            break;
        }
        result = virtua_nftw_visit(child, fn, flags, level + 1);
        free(child);
        if (result != 0) break;
    }
    closedir(dir);
    if (result != 0) return result;

    if (depth_first) return fn(path, &st, FTW_DP, &info);
    return 0;
}

int nftw(const char* path, __ftw_func_t fn, int fd_limit, int flags) {
    (void)fd_limit;
    if (!path || !fn) {
        errno = EINVAL;
        return -1;
    }
    return virtua_nftw_visit(path, fn, flags, 0);
}

char* realpath(const char* path, char* resolved) {
    if (!path) {
        errno = EINVAL;
        return nullptr;
    }
    char* out = resolved;
    if (!out) {
        out = static_cast<char*>(malloc(strlen(path) + 1));
        if (!out) {
            errno = ENOMEM;
            return nullptr;
        }
    }
    strcpy(out, path);
    return out;
}

int getpwuid_r(uid_t, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
    if (!pwd || !buf || buflen < 32 || !result) {
        if (result) *result = nullptr;
        return ERANGE;
    }
    char* cursor = buf;
    auto put = [&](const char* value) -> char* {
        const size_t len = strlen(value) + 1;
        if (static_cast<size_t>(cursor - buf) + len > buflen) return nullptr;
        char* slot = cursor;
        memcpy(slot, value, len);
        cursor += len;
        return slot;
    };
    pwd->pw_name = put("virtua");
    pwd->pw_passwd = put("");
    pwd->pw_gecos = put("Virtua");
    pwd->pw_dir = put("/");
    pwd->pw_shell = put("/System/Applications/Terminal.virtua");
    if (!pwd->pw_name || !pwd->pw_passwd || !pwd->pw_gecos || !pwd->pw_dir || !pwd->pw_shell) {
        *result = nullptr;
        return ERANGE;
    }
    pwd->pw_uid = 0;
    pwd->pw_gid = 0;
    *result = pwd;
    return 0;
}

int getpwnam_r(const char*, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
    return getpwuid_r(0, pwd, buf, buflen, result);
}

int statvfs(const char*, struct statvfs* buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_namemax = 255;
    return 0;
}

int fstatvfs(int, struct statvfs* buf) {
    return statvfs("/", buf);
}

int statfs(const char*, struct statfs* buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_iosize = 4096;
    buf->f_frsize = 4096;
    buf->f_flags = MNT_LOCAL;
    buf->f_namelen = 255;
    return 0;
}

int fstatfs(int, struct statfs* buf) {
    return statfs("/", buf);
}

int getrlimit(int, struct rlimit* limit) {
    if (!limit) {
        errno = EINVAL;
        return -1;
    }
    limit->rlim_cur = RLIM_INFINITY;
    limit->rlim_max = RLIM_INFINITY;
    return 0;
}

int setrlimit(int, const struct rlimit*) {
    return 0;
}

int getrusage(int, struct rusage* usage) {
    if (!usage) {
        errno = EINVAL;
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    return 0;
}

struct VirtuaMmapAllocation {
    void* ptr;
    size_t size;
    int backing_fd;
    off_t offset;
    bool write_back;
};

static VirtuaMmapAllocation g_virtua_mmap_allocations[256];

static void track_heap_mmap(void* ptr, size_t size, int backing_fd, off_t offset, bool write_back) {
    if (!ptr) return;
    for (VirtuaMmapAllocation& allocation : g_virtua_mmap_allocations) {
        if (!allocation.ptr) {
            allocation.ptr = ptr;
            allocation.size = size;
            allocation.backing_fd = backing_fd;
            allocation.offset = offset;
            allocation.write_back = write_back;
            return;
        }
    }
}

static bool untrack_heap_mmap(void* ptr, VirtuaMmapAllocation* result) {
    for (VirtuaMmapAllocation& allocation : g_virtua_mmap_allocations) {
        if (allocation.ptr == ptr) {
            if (result) *result = allocation;
            allocation = {};
            return true;
        }
    }
    return false;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!length) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    const bool file_backed = (fd >= 0 && (flags & MAP_ANONYMOUS) == 0);
    bool fixed = (flags & MAP_FIXED) != 0;
#ifdef MAP_FIXED_NOREPLACE
    fixed = fixed || (flags & MAP_FIXED_NOREPLACE) != 0;
#endif

    if (addr && !fixed && !file_backed && prot == PROT_NONE) {
        return addr;
    }

    const bool write_back = file_backed && (prot & PROT_WRITE) && (flags & MAP_SHARED);
    int backing_fd = -1;
    if (write_back && (backing_fd = dup(fd)) < 0) {
        return MAP_FAILED;
    }

    if (file_backed && (prot & PROT_WRITE) && !fixed && !write_back) {
        errno = ENODEV;
        return MAP_FAILED;
    }

    if (fixed) {
        if (!addr) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (!file_backed) {
            if ((prot & (PROT_READ | PROT_WRITE)) && length <= 16u * 1024u * 1024u) {
                memset(addr, 0, length);
            }
            return addr;
        }

        if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) {
            errno = ENODEV;
            return MAP_FAILED;
        }

        memset(addr, 0, length);
        const long saved = sys_lseek(fd, 0, SEEK_CUR);
        if (sys_lseek(fd, static_cast<long>(offset), SEEK_SET) >= 0) {
            size_t total = 0;
            while (total < length) {
                long r = sys_read(fd, static_cast<char*>(addr) + total, length - total);
                if (r <= 0) break;
                total += static_cast<size_t>(r);
            }
        }
        if (saved >= 0) sys_lseek(fd, saved, SEEK_SET);
        return addr;
    }

    // Allocate one extra byte so a populated file mapping carries the trailing
    // NUL that LLVM's MemoryBuffer mmap path reads past the mapped length.
    const size_t alloc = length + 1;
    void* p = malloc(alloc);
    if (!p) {
        if (backing_fd >= 0) close(backing_fd);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    track_heap_mmap(p, length, backing_fd, offset, write_back);
    memset(p, 0, alloc);

    // Read-only file mappings must be populated with the file's bytes —
    // otherwise callers that mmap an input file (e.g. LLVM MemoryBuffer reading
    // archives or objects once they cross the page-size threshold) observe
    // uninitialised memory and reject the file as "unknown file type".
    // Anonymous mappings (fd < 0 or MAP_ANONYMOUS) stay zero-filled scratch,
    // exactly as the malloc heap and JIT regions expect.
    if (length != 0 && file_backed) {
        const long saved = sys_lseek(fd, 0, SEEK_CUR);
        if (sys_lseek(fd, static_cast<long>(offset), SEEK_SET) >= 0) {
            size_t total = 0;
            while (total < length) {
                long r = sys_read(fd, static_cast<char*>(p) + total, length - total);
                if (r <= 0) break;
                total += static_cast<size_t>(r);
            }
        }
        if (saved >= 0) sys_lseek(fd, saved, SEEK_SET);
    }
    return p;
}

int munmap(void* addr, size_t) {
    VirtuaMmapAllocation allocation {};
    if (!untrack_heap_mmap(addr, &allocation)) return 0;

    int result = 0;
    if (allocation.write_back) {
        size_t total = 0;
        while (total < allocation.size) {
            const long written = sys_pwrite(allocation.backing_fd,
                                            static_cast<const char*>(allocation.ptr) + total,
                                            allocation.size - total,
                                            static_cast<long>(allocation.offset + total));
            if (written <= 0) {
                errno = written < 0 ? static_cast<int>(-written) : EIO;
                result = -1;
                break;
            }
            total += static_cast<size_t>(written);
        }
    }
    if (allocation.backing_fd >= 0) close(allocation.backing_fd);
    free(allocation.ptr);
    return result;
}

// Emulated TLS runtime.  The Virtua launcher runs guest code with no usable %fs
// base, so native (%fs-relative) ELF thread-local storage faults at address 0.
// The toolchain therefore builds with -femulated-tls, which routes every
// thread_local through __emutls_get_address.  The runtime is single-threaded
// (LLVM_ENABLE_THREADS=OFF), so each control object owns exactly one instance:
// allocate it on first use, initialise from the template (or zero), and cache
// the pointer in the control's object slot.  The storage lives for the whole
// program, so it is intentionally never freed.
namespace {
struct VirtuaEmutlsControl {
    size_t size;
    size_t align;
    void*  object;  // union { uintptr_t index; void* address; }
    void*  value;   // initialisation template, or null for zero-init
};
}

extern "C" void* __emutls_get_address(void* control) {
    VirtuaEmutlsControl* c = static_cast<VirtuaEmutlsControl*>(control);
    if (!c->object) {
        size_t align = c->align ? c->align : 1;
        char* raw = static_cast<char*>(malloc(c->size + align));
        if (!raw) return nullptr;
        uintptr_t aligned =
            (reinterpret_cast<uintptr_t>(raw) + (align - 1)) & ~(static_cast<uintptr_t>(align) - 1);
        void* obj = reinterpret_cast<void*>(aligned);
        if (c->value) {
            memcpy(obj, c->value, c->size);
        } else {
            memset(obj, 0, c->size);
        }
        c->object = obj;
    }
    return c->object;
}

int mprotect(void*, size_t, int) {
    return 0;
}

int msync(void*, size_t, int) {
    return 0;
}

int madvise(void*, size_t, int) {
    return 0;
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    long rc = sys_poll(fds, nfds, timeout_ms);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return static_cast<int>(rc);
}

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout) {
    long rc = sys_select(nfds, readfds, writefds, exceptfds, timeout);
    if (rc < 0) {
        errno = static_cast<int>(-rc);
        return -1;
    }
    return static_cast<int>(rc);
}

VIRTUA_WEAK_SYMBOL int ioctl(int, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    if (request == TIOCGWINSZ) {
        auto* ws = va_arg(args, struct winsize*);
        if (ws) {
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            va_end(args);
            return 0;
        }
    }
    va_end(args);
    errno = ENOTTY;
    return -1;
}

VIRTUA_WEAK_SYMBOL int usleep(useconds_t usec) {
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(usec / 1000000U);
    ts.tv_nsec = static_cast<long>((usec % 1000000U) * 1000U);
    return nanosleep(&ts, nullptr);
}

VIRTUA_WEAK_SYMBOL ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt < 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        const long rc = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) {
            errno = static_cast<int>(-rc);
            return total > 0 ? total : -1;
        }
        total += static_cast<ssize_t>(rc);
        if (static_cast<size_t>(rc) != iov[i].iov_len) {
            break;
        }
    }
    return total;
}

VIRTUA_WEAK_SYMBOL ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt < 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        const long rc = sys_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) {
            errno = static_cast<int>(-rc);
            return total > 0 ? total : -1;
        }
        if (rc == 0) break;
        total += static_cast<ssize_t>(rc);
        if (static_cast<size_t>(rc) != iov[i].iov_len) {
            break;
        }
    }
    return total;
}

VIRTUA_WEAK_SYMBOL ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    const long saved = sys_lseek(fd, 0, SEEK_CUR);
    if (sys_lseek(fd, static_cast<long>(offset), SEEK_SET) < 0) {
        errno = EINVAL;
        return -1;
    }
    const ssize_t rc = writev(fd, iov, iovcnt);
    if (saved >= 0) sys_lseek(fd, saved, SEEK_SET);
    return rc;
}

VIRTUA_WEAK_SYMBOL ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    const long saved = sys_lseek(fd, 0, SEEK_CUR);
    if (sys_lseek(fd, static_cast<long>(offset), SEEK_SET) < 0) {
        errno = EINVAL;
        return -1;
    }
    const ssize_t rc = readv(fd, iov, iovcnt);
    if (saved >= 0) sys_lseek(fd, saved, SEEK_SET);
    return rc;
}

VIRTUA_WEAK_SYMBOL FILE* popen(const char*, const char*) {
    errno = ENOSYS;
    return nullptr;
}

VIRTUA_WEAK_SYMBOL int pclose(FILE*) {
    errno = ENOSYS;
    return -1;
}

VIRTUA_WEAK_SYMBOL int sigsetjmp(jmp_buf env, int) throw() __attribute__((returns_twice));
VIRTUA_WEAK_SYMBOL int sigsetjmp(jmp_buf env, int) throw() {
    return setjmp(env);
}

VIRTUA_WEAK_SYMBOL void siglongjmp(jmp_buf env, int value) throw() __attribute__((noreturn));
VIRTUA_WEAK_SYMBOL void siglongjmp(jmp_buf env, int value) throw() {
    longjmp(env, value == 0 ? 1 : value);
    spin_after_exit();
}

pid_t wait4(pid_t pid, int* status, int options, struct rusage* usage) {
    if (usage) {
        memset(usage, 0, sizeof(*usage));
    }
    return waitpid(pid, status, options);
}

char* strsignal(int sig) {
    static char buffer[32];
    snprintf(buffer, sizeof(buffer), "signal %d", sig);
    return buffer;
}

int uname(struct utsname* name) {
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    memset(name, 0, sizeof(*name));
    strncpy(name->sysname, "Virtua", sizeof(name->sysname) - 1);
    strncpy(name->nodename, "virtua", sizeof(name->nodename) - 1);
    strncpy(name->release, "1.0", sizeof(name->release) - 1);
    strncpy(name->version, "MVII", sizeof(name->version) - 1);
#if defined(__x86_64__)
    strncpy(name->machine, "x86_64", sizeof(name->machine) - 1);
#elif defined(__aarch64__)
    strncpy(name->machine, "aarch64", sizeof(name->machine) - 1);
#elif defined(__riscv)
    strncpy(name->machine, "riscv64", sizeof(name->machine) - 1);
#else
    strncpy(name->machine, "unknown", sizeof(name->machine) - 1);
#endif
    return 0;
}

pid_t wait(int* status) {
    return waitpid(-1, status, 0);
}

pid_t waitpid(pid_t, int* status, int) {
    if (status) {
        *status = 0;
    }
    errno = ECHILD;
    return -1;
}

VIRTUA_WEAK_SYMBOL void* dlopen(const char*, int) {
    errno = ENOSYS;
    return nullptr;
}

VIRTUA_WEAK_SYMBOL void* dlsym(void*, const char*) {
    errno = ENOSYS;
    return nullptr;
}

VIRTUA_WEAK_SYMBOL int dlclose(void*) {
    return 0;
}

char* dlerror(void) {
    static char message[] = "dynamic loading is unavailable in this Virtua runtime";
    return message;
}

int dladdr(const void*, Dl_info* info) {
    if (!info) {
        errno = EINVAL;
        return 0;
    }
    info->dli_fname = "virtua";
    info->dli_fbase = nullptr;
    info->dli_sname = nullptr;
    info->dli_saddr = nullptr;
    return 1;
}

#if defined(__arm__)
VIRTUA_WEAK_SYMBOL void __aeabi_memcpy(void* dst, const void* src, size_t count) {
    memcpy(dst, src, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memcpy4(void* dst, const void* src, size_t count) {
    memcpy(dst, src, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memcpy8(void* dst, const void* src, size_t count) {
    memcpy(dst, src, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memmove(void* dst, const void* src, size_t count) {
    memmove(dst, src, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memmove4(void* dst, const void* src, size_t count) {
    memmove(dst, src, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memmove8(void* dst, const void* src, size_t count) {
    memmove(dst, src, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memset(void* dst, size_t count, int value) {
    memset(dst, value, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memset4(void* dst, size_t count, int value) {
    memset(dst, value, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memset8(void* dst, size_t count, int value) {
    memset(dst, value, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memclr(void* dst, size_t count) {
    memset(dst, 0, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memclr4(void* dst, size_t count) {
    memset(dst, 0, count);
}

VIRTUA_WEAK_SYMBOL void __aeabi_memclr8(void* dst, size_t count) {
    memset(dst, 0, count);
}

VIRTUA_WEAK_SYMBOL int __aeabi_idiv0(int value) {
    return value;
}

VIRTUA_WEAK_SYMBOL long long __aeabi_ldiv0(long long value) {
    return value;
}

VIRTUA_WEAK_SYMBOL unsigned int __aeabi_uidiv(unsigned int lhs, unsigned int rhs) {
    return divmod_u32(lhs, rhs, nullptr);
}

VIRTUA_WEAK_SYMBOL int __aeabi_idiv(int lhs, int rhs) {
    if (rhs == 0) return __aeabi_idiv0(0);
    const bool     negative = (lhs < 0) != (rhs < 0);
    const uint32_t quotient = divmod_u32(abs_i32_to_u32(lhs), abs_i32_to_u32(rhs), nullptr);
    return apply_i32_sign(quotient, negative);
}

unsigned int __power_aeabi_uidivmod_impl(unsigned int lhs, unsigned int rhs, unsigned int* remainder) {
    return divmod_u32(lhs, rhs, remainder);
}

int __power_aeabi_idivmod_impl(int lhs, int rhs, int* remainder) {
    if (rhs == 0) {
        if (remainder) *remainder = lhs;
        return __aeabi_idiv0(0);
    }

    uint32_t   unsigned_remainder = 0;
    const bool negative           = (lhs < 0) != (rhs < 0);
    const uint32_t quotient       = divmod_u32(abs_i32_to_u32(lhs), abs_i32_to_u32(rhs), &unsigned_remainder);
    if (remainder) {
        *remainder = apply_i32_sign(unsigned_remainder, lhs < 0);
    }
    return apply_i32_sign(quotient, negative);
}

unsigned long long __power_aeabi_uldivmod_impl(unsigned long long lhs,
                                               unsigned long long rhs,
                                               unsigned long long* remainder) {
    return divmod_u64(lhs, rhs, remainder);
}

long long __power_aeabi_ldivmod_impl(long long lhs, long long rhs, long long* remainder) {
    if (rhs == 0) {
        if (remainder) *remainder = lhs;
        return __aeabi_ldiv0(0);
    }

    uint64_t   unsigned_remainder = 0;
    const bool negative           = (lhs < 0) != (rhs < 0);
    const uint64_t quotient       = divmod_u64(abs_i64_to_u64(lhs), abs_i64_to_u64(rhs), &unsigned_remainder);
    if (remainder) {
        *remainder = apply_i64_sign(unsigned_remainder, lhs < 0);
    }
    return apply_i64_sign(quotient, negative);
}

VIRTUA_WEAK_SYMBOL float __aeabi_fadd(float lhs, float rhs) {
    return lhs + rhs;
}

VIRTUA_WEAK_SYMBOL float __aeabi_fsub(float lhs, float rhs) {
    return lhs - rhs;
}

VIRTUA_WEAK_SYMBOL float __aeabi_frsub(float lhs, float rhs) {
    return rhs - lhs;
}

VIRTUA_WEAK_SYMBOL float __aeabi_fmul(float lhs, float rhs) {
    return lhs * rhs;
}

VIRTUA_WEAK_SYMBOL float __aeabi_fdiv(float lhs, float rhs) {
    return lhs / rhs;
}

VIRTUA_WEAK_SYMBOL double __aeabi_f2d(float value) {
    return static_cast<double>(value);
}

VIRTUA_WEAK_SYMBOL int __aeabi_f2iz(float value) {
    return static_cast<int>(value);
}

VIRTUA_WEAK_SYMBOL unsigned int __aeabi_f2uiz(float value) {
    return static_cast<unsigned int>(value);
}

VIRTUA_WEAK_SYMBOL long long __aeabi_f2lz(float value) {
    return float_to_i64(value);
}

VIRTUA_WEAK_SYMBOL unsigned long long __aeabi_f2ulz(float value) {
    return float_to_u64(value);
}

VIRTUA_WEAK_SYMBOL float __aeabi_i2f(int value) {
    return static_cast<float>(value);
}

VIRTUA_WEAK_SYMBOL float __aeabi_ui2f(unsigned int value) {
    return static_cast<float>(value);
}

VIRTUA_WEAK_SYMBOL float __aeabi_l2f(long long value) {
    return static_cast<float>(i64_to_double(value));
}

VIRTUA_WEAK_SYMBOL float __aeabi_ul2f(unsigned long long value) {
    return static_cast<float>(u64_to_double(value));
}

VIRTUA_WEAK_SYMBOL int __aeabi_fcmpeq(float lhs, float rhs) {
    return lhs == rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_fcmplt(float lhs, float rhs) {
    return lhs < rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_fcmple(float lhs, float rhs) {
    return lhs <= rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_fcmpge(float lhs, float rhs) {
    return lhs >= rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_fcmpgt(float lhs, float rhs) {
    return lhs > rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_fcmpun(float lhs, float rhs) {
    return (lhs != lhs) || (rhs != rhs);
}

VIRTUA_WEAK_SYMBOL double __aeabi_dadd(double lhs, double rhs) {
    return lhs + rhs;
}

VIRTUA_WEAK_SYMBOL double __aeabi_dsub(double lhs, double rhs) {
    return lhs - rhs;
}

VIRTUA_WEAK_SYMBOL double __aeabi_drsub(double lhs, double rhs) {
    return rhs - lhs;
}

VIRTUA_WEAK_SYMBOL double __aeabi_dmul(double lhs, double rhs) {
    return lhs * rhs;
}

VIRTUA_WEAK_SYMBOL double __aeabi_ddiv(double lhs, double rhs) {
    return lhs / rhs;
}

VIRTUA_WEAK_SYMBOL float __aeabi_d2f(double value) {
    return static_cast<float>(value);
}

VIRTUA_WEAK_SYMBOL int __aeabi_d2iz(double value) {
    return static_cast<int>(value);
}

VIRTUA_WEAK_SYMBOL unsigned int __aeabi_d2uiz(double value) {
    return static_cast<unsigned int>(value);
}

VIRTUA_WEAK_SYMBOL long long __aeabi_d2lz(double value) {
    return double_to_i64(value);
}

VIRTUA_WEAK_SYMBOL unsigned long long __aeabi_d2ulz(double value) {
    return double_to_u64(value);
}

VIRTUA_WEAK_SYMBOL double __aeabi_i2d(int value) {
    return static_cast<double>(value);
}

VIRTUA_WEAK_SYMBOL double __aeabi_ui2d(unsigned int value) {
    return static_cast<double>(value);
}

VIRTUA_WEAK_SYMBOL double __aeabi_l2d(long long value) {
    return i64_to_double(value);
}

VIRTUA_WEAK_SYMBOL double __aeabi_ul2d(unsigned long long value) {
    return u64_to_double(value);
}

VIRTUA_WEAK_SYMBOL int __aeabi_dcmpeq(double lhs, double rhs) {
    return lhs == rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_dcmplt(double lhs, double rhs) {
    return lhs < rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_dcmple(double lhs, double rhs) {
    return lhs <= rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_dcmpge(double lhs, double rhs) {
    return lhs >= rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_dcmpgt(double lhs, double rhs) {
    return lhs > rhs;
}

VIRTUA_WEAK_SYMBOL int __aeabi_dcmpun(double lhs, double rhs) {
    return (lhs != lhs) || (rhs != rhs);
}

__asm__(
    ".syntax unified\n"
    ".text\n"
    ".global __aeabi_uidivmod\n"
    ".type __aeabi_uidivmod,%function\n"
    "__aeabi_uidivmod:\n"
    "    push {r2, lr}\n"
    "    mov r2, sp\n"
    "    bl __power_aeabi_uidivmod_impl\n"
    "    ldr r1, [sp]\n"
    "    pop {r2, pc}\n"
    ".global __aeabi_idivmod\n"
    ".type __aeabi_idivmod,%function\n"
    "__aeabi_idivmod:\n"
    "    push {r2, lr}\n"
    "    mov r2, sp\n"
    "    bl __power_aeabi_idivmod_impl\n"
    "    ldr r1, [sp]\n"
    "    pop {r2, pc}\n"
    ".global __aeabi_uldivmod\n"
    ".type __aeabi_uldivmod,%function\n"
    "__aeabi_uldivmod:\n"
    "    push {r4, lr}\n"
    "    sub sp, sp, #8\n"
    "    mov r4, sp\n"
    "    sub sp, sp, #8\n"
    "    str r4, [sp]\n"
    "    bl __power_aeabi_uldivmod_impl\n"
    "    add sp, sp, #8\n"
    "    ldr r2, [sp]\n"
    "    ldr r3, [sp, #4]\n"
    "    add sp, sp, #8\n"
    "    pop {r4, pc}\n"
    ".global __aeabi_ldivmod\n"
    ".type __aeabi_ldivmod,%function\n"
    "__aeabi_ldivmod:\n"
    "    push {r4, lr}\n"
    "    sub sp, sp, #8\n"
    "    mov r4, sp\n"
    "    sub sp, sp, #8\n"
    "    str r4, [sp]\n"
    "    bl __power_aeabi_ldivmod_impl\n"
    "    add sp, sp, #8\n"
    "    ldr r2, [sp]\n"
    "    ldr r3, [sp, #4]\n"
    "    add sp, sp, #8\n"
    "    pop {r4, pc}\n");
#endif

#if defined(__SIZEOF_INT128__)
VIRTUA_WEAK_SYMBOL VirtuaU128 __udivti3(VirtuaU128 lhs, VirtuaU128 rhs) {
    return divmod_u128(lhs, rhs, nullptr);
}

VIRTUA_WEAK_SYMBOL VirtuaU128 __umodti3(VirtuaU128 lhs, VirtuaU128 rhs) {
    VirtuaU128 remainder = 0;
    divmod_u128(lhs, rhs, &remainder);
    return remainder;
}

VIRTUA_WEAK_SYMBOL VirtuaU128 __udivmodti4(VirtuaU128 lhs, VirtuaU128 rhs, VirtuaU128* remainder) {
    return divmod_u128(lhs, rhs, remainder);
}

VIRTUA_WEAK_SYMBOL VirtuaS128 __divti3(VirtuaS128 lhs, VirtuaS128 rhs) {
    const bool negative = (lhs < 0) != (rhs < 0);
    const VirtuaU128 quotient = divmod_u128(abs_i128(lhs), abs_i128(rhs), nullptr);
    return negative ? -static_cast<VirtuaS128>(quotient) : static_cast<VirtuaS128>(quotient);
}

VIRTUA_WEAK_SYMBOL VirtuaS128 __modti3(VirtuaS128 lhs, VirtuaS128 rhs) {
    VirtuaU128 remainder = 0;
    divmod_u128(abs_i128(lhs), abs_i128(rhs), &remainder);
    const VirtuaS128 signed_remainder = static_cast<VirtuaS128>(remainder);
    return lhs < 0 ? -signed_remainder : signed_remainder;
}
#endif

// libunwind references these markers.  In C++ a file-scope `const` has
// internal linkage by default - force external linkage with extern and
// __attribute__((used,retain)) so neither the compiler nor
// --gc-sections drops them.  Pong is freestanding and never throws, so
// empty placeholders are sufficient.
extern "C" __attribute__((used, retain)) const unsigned char __eh_frame_start[1]     = {0};
extern "C" __attribute__((used, retain)) const unsigned char __eh_frame_end[1]       = {0};
extern "C" __attribute__((used, retain)) const unsigned char __eh_frame_hdr_start[1] = {0};
extern "C" __attribute__((used, retain)) const unsigned char __eh_frame_hdr_end[1]   = {0};

} // extern "C"

namespace __llvm_libc_22_1_3_ {

[[noreturn]] void abort(void) {
    ::abort();
}

[[noreturn]] void __llvm_libc_exit(int status) {
    ::__llvm_libc_exit(status);
}

int timespec_get(::timespec* ts, int base) {
    return ::timespec_get(ts, base);
}

} // namespace __llvm_libc_22_1_3_
