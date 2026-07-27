/*
 * MVII baremetal POSIX shim - minimal pthread API.
 *
 * This is a small pthread-compatible surface for Virtua/MVII userland. The
 * launcher/runtime may back these with host threads while the kernel port
 * grows native scheduling.
 */
#ifndef _POSIX_SHIM_PTHREAD_H
#define _POSIX_SHIM_PTHREAD_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t pthread_t;
typedef unsigned int pthread_key_t;
typedef int pthread_once_t;
typedef volatile int minos_spinlock_t;

typedef struct pthread_mutexattr_t {
    int type;
} pthread_mutexattr_t;

typedef struct pthread_mutex_t {
    int initialized;
    int type;
    minos_spinlock_t locked;
    unsigned int recursion;
    pthread_t owner;
} pthread_mutex_t;

typedef struct pthread_cond_t {
    int initialized;
    volatile unsigned int sequence;
} pthread_cond_t;

typedef struct pthread_condattr_t {
    clockid_t clock_id;
} pthread_condattr_t;

typedef struct pthread_attr_t {
    size_t stack_size;
    int detach_state;
} pthread_attr_t;

typedef struct pthread_rwlock_t {
    pthread_mutex_t mutex;
} pthread_rwlock_t;

#ifndef PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_NORMAL 0
#endif
#ifndef PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_RECURSIVE 1
#endif
#ifndef PTHREAD_MUTEX_ERRORCHECK
#define PTHREAD_MUTEX_ERRORCHECK 2
#endif
#ifndef PTHREAD_MUTEX_DEFAULT
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#endif

#define PTHREAD_MUTEX_INITIALIZER {1, PTHREAD_MUTEX_NORMAL, 0, 0, 0}
#ifndef PTHREAD_CREATE_JOINABLE
#define PTHREAD_CREATE_JOINABLE 0
#endif
#ifndef PTHREAD_CREATE_DETACHED
#define PTHREAD_CREATE_DETACHED 1
#endif

#define PTHREAD_COND_INITIALIZER {1, 0}
#define PTHREAD_RWLOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER}
#define PTHREAD_ONCE_INIT 0

int pthread_mutexattr_init(pthread_mutexattr_t* attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t* attr);
int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type);

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_broadcast(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime);
int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t clock_id, const struct timespec* abstime);
int pthread_condattr_init(pthread_condattr_t* attr);
int pthread_condattr_destroy(pthread_condattr_t* attr);
int pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock_id);
int pthread_condattr_getclock(const pthread_condattr_t* attr, clockid_t* clock_id);

int pthread_rwlock_init(pthread_rwlock_t* lock, const void* attr);
int pthread_rwlock_destroy(pthread_rwlock_t* lock);
int pthread_rwlock_rdlock(pthread_rwlock_t* lock);
int pthread_rwlock_wrlock(pthread_rwlock_t* lock);
int pthread_rwlock_unlock(pthread_rwlock_t* lock);

int pthread_once(pthread_once_t* once_control, void (*init_routine)(void));

int pthread_attr_init(pthread_attr_t* attr);
int pthread_attr_destroy(pthread_attr_t* attr);
int pthread_attr_setdetachstate(pthread_attr_t* attr, int detach_state);
int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detach_state);
int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stack_size);
int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* stack_size);

pthread_t pthread_self(void);
int pthread_equal(pthread_t lhs, pthread_t rhs);
int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start_routine)(void*), void* arg);
int pthread_join(pthread_t thread, void** retval);
int pthread_detach(pthread_t thread);
void pthread_exit(void* retval);

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t key);
void* pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void* value);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_PTHREAD_H */
