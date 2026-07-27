#include "minos_user_abi.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

extern "C" long sys_gettimeofday(void *tv, void *tz);
extern "C" long sys_thread_create(uintptr_t *thread, uintptr_t entry,
                                  uintptr_t arg, size_t stack_size);
extern "C" long sys_thread_join(uintptr_t thread, void **retval);
extern "C" long sys_thread_detach(uintptr_t thread);
extern "C" uintptr_t sys_thread_self(void);
extern "C" long sys_thread_yield(void);
extern "C" long sys_sleep_until_us(uint64_t deadline_us);
extern "C" void sys_exit(int code);

namespace {

constexpr unsigned kTlsKeyCount = 64;
constexpr unsigned kTlsThreadCount = 32;

struct TlsKey {
    bool used;
    void (*destructor)(void *);
};

struct TlsThread {
    bool used;
    pthread_t id;
    const void *values[kTlsKeyCount];
};

static TlsKey tls_keys[kTlsKeyCount];
static TlsThread tls_threads[kTlsThreadCount];
static volatile int tls_lock;
static thread_local int thread_errno;

static uint64_t realtime_microseconds()
{
    struct timeval { long tv_sec; long tv_usec; } value{};
    if (sys_gettimeofday(&value, nullptr) < 0) return 0;
    return (uint64_t)value.tv_sec * 1000000ull + (uint64_t)value.tv_usec;
}

static void yield_briefly(uint64_t deadline = 0)
{
    const uint64_t now = realtime_microseconds();
    uint64_t target = now ? now + 1000ull : 0;
    if (deadline && (!target || deadline < target)) target = deadline;
    if (!target || sys_sleep_until_us(target) != 0) (void)sys_thread_yield();
}

static void lock_tls()
{
    while (__atomic_test_and_set(&tls_lock, __ATOMIC_ACQUIRE))
        yield_briefly();
}

static void unlock_tls()
{
    __atomic_clear(&tls_lock, __ATOMIC_RELEASE);
}

static TlsThread *tls_thread(bool create)
{
    const pthread_t id = (pthread_t)sys_thread_self();
    TlsThread *free_slot = nullptr;
    for (auto &slot : tls_threads) {
        if (slot.used && slot.id == id) return &slot;
        if (!slot.used && !free_slot) free_slot = &slot;
    }
    if (!create || !free_slot) return nullptr;
    free_slot->used = true;
    free_slot->id = id;
    memset(free_slot->values, 0, sizeof(free_slot->values));
    return free_slot;
}

static void ensure_mutex(pthread_mutex_t *mutex)
{
    if (mutex && !mutex->initialized) {
        mutex->initialized = 1;
        mutex->type = PTHREAD_MUTEX_NORMAL;
        mutex->locked = 0;
        mutex->recursion = 0;
        mutex->owner = 0;
    }
}

static uint64_t timespec_microseconds(const struct timespec *value)
{
    return value ? (uint64_t)value->tv_sec * 1000000ull +
                   (uint64_t)(value->tv_nsec / 1000l) : 0;
}

} // namespace

extern "C" {

int *__llvm_libc_errno(void) noexcept { return &thread_errno; }

int pthread_mutexattr_init(pthread_mutexattr_t *attr) { if (!attr) return EINVAL; attr->type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) { return attr ? 0 : EINVAL; }
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) { if (!attr || (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE && type != PTHREAD_MUTEX_ERRORCHECK)) return EINVAL; attr->type = type; return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) { if (!attr || !type) return EINVAL; *type = attr->type; return 0; }

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    if (!mutex) return EINVAL;
    mutex->initialized = 1;
    mutex->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
    mutex->locked = 0;
    mutex->recursion = 0;
    mutex->owner = 0;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *mutex) { if (!mutex) return EINVAL; if (__atomic_load_n(&mutex->locked, __ATOMIC_ACQUIRE)) return EBUSY; mutex->initialized = 0; return 0; }
int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    ensure_mutex(mutex);
    const pthread_t self = pthread_self();
    if (__atomic_load_n(&mutex->locked, __ATOMIC_ACQUIRE) && mutex->owner == self) {
        if (mutex->type == PTHREAD_MUTEX_RECURSIVE) { ++mutex->recursion; return 0; }
        if (mutex->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
    }
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&mutex->locked, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            mutex->owner = self;
            mutex->recursion = 1;
            return 0;
        }
        yield_briefly();
    }
}
int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    ensure_mutex(mutex);
    const pthread_t self = pthread_self();
    int expected = 0;
    if (__atomic_compare_exchange_n(&mutex->locked, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        mutex->owner = self; mutex->recursion = 1; return 0;
    }
    if (mutex->owner == self && mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        ++mutex->recursion; return 0;
    }
    return EBUSY;
}
int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    ensure_mutex(mutex);
    if (!mutex->locked || mutex->owner != pthread_self()) return EPERM;
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->recursion > 1) {
        --mutex->recursion; return 0;
    }
    mutex->recursion = 0; mutex->owner = 0;
    __atomic_store_n(&mutex->locked, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *) { if (!condition) return EINVAL; condition->initialized = 1; condition->sequence = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *condition) { if (!condition) return EINVAL; condition->initialized = 0; return 0; }
int pthread_cond_signal(pthread_cond_t *condition) { if (!condition) return EINVAL; __atomic_add_fetch(&condition->sequence, 1u, __ATOMIC_RELEASE); return 0; }
int pthread_cond_broadcast(pthread_cond_t *condition) { return pthread_cond_signal(condition); }
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    if (!condition || !mutex) return EINVAL;
    const unsigned sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
    (void)pthread_mutex_unlock(mutex);
    while (__atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE) == sequence)
        yield_briefly();
    return pthread_mutex_lock(mutex);
}
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                           const struct timespec *deadline)
{
    if (!condition || !mutex || !deadline) return EINVAL;
    const unsigned sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
    const uint64_t target = timespec_microseconds(deadline);
    (void)pthread_mutex_unlock(mutex);
    while (__atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE) == sequence) {
        if (realtime_microseconds() >= target) {
            (void)pthread_mutex_lock(mutex);
            return ETIMEDOUT;
        }
        yield_briefly(target);
    }
    return pthread_mutex_lock(mutex);
}
int pthread_cond_clockwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                           clockid_t, const struct timespec *deadline)
{ return pthread_cond_timedwait(condition, mutex, deadline); }
int pthread_condattr_init(pthread_condattr_t *attr) { if (!attr) return EINVAL; attr->clock_id = CLOCK_REALTIME; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *attr) { return attr ? 0 : EINVAL; }
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t id) { if (!attr) return EINVAL; attr->clock_id = id; return 0; }
int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *id) { if (!attr || !id) return EINVAL; *id = attr->clock_id; return 0; }

int pthread_rwlock_init(pthread_rwlock_t *lock, const void *) { return lock ? pthread_mutex_init(&lock->mutex, nullptr) : EINVAL; }
int pthread_rwlock_destroy(pthread_rwlock_t *lock) { return lock ? pthread_mutex_destroy(&lock->mutex) : EINVAL; }
int pthread_rwlock_rdlock(pthread_rwlock_t *lock) { return lock ? pthread_mutex_lock(&lock->mutex) : EINVAL; }
int pthread_rwlock_wrlock(pthread_rwlock_t *lock) { return lock ? pthread_mutex_lock(&lock->mutex) : EINVAL; }
int pthread_rwlock_unlock(pthread_rwlock_t *lock) { return lock ? pthread_mutex_unlock(&lock->mutex) : EINVAL; }

int pthread_once(pthread_once_t *once, void (*function)(void))
{
    if (!once || !function) return EINVAL;
    int expected = 0;
    if (__atomic_compare_exchange_n(once, &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        function();
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 0) yield_briefly();
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr) { if (!attr) return EINVAL; attr->stack_size = 0; attr->detach_state = PTHREAD_CREATE_JOINABLE; return 0; }
int pthread_attr_destroy(pthread_attr_t *attr) { return attr ? 0 : EINVAL; }
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) { if (!attr || (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)) return EINVAL; attr->detach_state = state; return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state) { if (!attr || !state) return EINVAL; *state = attr->detach_state; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) { if (!attr || size < 4096) return EINVAL; attr->stack_size = size; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size) { if (!attr || !size) return EINVAL; *size = attr->stack_size; return 0; }

pthread_t pthread_self(void) { return (pthread_t)sys_thread_self(); }
int pthread_equal(pthread_t left, pthread_t right) { return left == right; }
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*function)(void *), void *argument)
{
    if (!thread || !function) return EINVAL;
    uintptr_t created = 0;
    const long result = sys_thread_create(&created, (uintptr_t)function,
                                          (uintptr_t)argument,
                                          attr ? attr->stack_size : 0);
    if (result < 0) return (int)-result;
    *thread = created;
    if (attr && attr->detach_state == PTHREAD_CREATE_DETACHED)
        return pthread_detach(*thread);
    return 0;
}
int pthread_join(pthread_t thread, void **result) { const long rc = sys_thread_join(thread, result); return rc < 0 ? (int)-rc : (int)rc; }
int pthread_detach(pthread_t thread) { const long rc = sys_thread_detach(thread); return rc < 0 ? (int)-rc : (int)rc; }
void pthread_exit(void *) { sys_exit(126); for (;;) (void)sys_thread_yield(); }

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    if (!key) return EINVAL;
    lock_tls();
    for (unsigned index = 0; index < kTlsKeyCount; ++index) {
        if (!tls_keys[index].used) {
            tls_keys[index].used = true;
            tls_keys[index].destructor = destructor;
            *key = index;
            unlock_tls();
            return 0;
        }
    }
    unlock_tls();
    return EAGAIN;
}
int pthread_key_delete(pthread_key_t key)
{
    if (key >= kTlsKeyCount) return EINVAL;
    lock_tls();
    tls_keys[key] = {};
    for (auto &thread : tls_threads) thread.values[key] = nullptr;
    unlock_tls();
    return 0;
}
void *pthread_getspecific(pthread_key_t key)
{
    if (key >= kTlsKeyCount) return nullptr;
    lock_tls();
    TlsThread *thread = tls_thread(false);
    void *value = thread ? const_cast<void *>(thread->values[key]) : nullptr;
    unlock_tls();
    return value;
}
int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= kTlsKeyCount || !tls_keys[key].used) return EINVAL;
    lock_tls();
    TlsThread *thread = tls_thread(true);
    if (!thread) { unlock_tls(); return EAGAIN; }
    thread->values[key] = value;
    unlock_tls();
    return 0;
}

int sched_yield(void) { const long rc = sys_thread_yield(); return rc < 0 ? (int)-rc : 0; }

} // extern "C"
