/*
 * stubs.c
 *
 * Provides empty stub functions to satisfy the linker when
 * linking against a C library (like newlib) that expects
 * threading and lock support, which minos-vi (xv6) does not provide.
 *
 * Compile this file and link it into your final executable.
 */

/*
 * This is a minimal "lock" structure. We just need to define
 * the type so the functions can accept it as a parameter.
 * It doesn't need to hold any real data.
 */
typedef int _lock_t;

/*
 * pthread stubs
 * The C library is trying to manage thread cancellation.
 * We can stub these functions to do nothing.
 */
void pthread_setcancelstate(int state, int *oldstate) {
    if(oldstate) {
        *oldstate = 0;
    }
}

/*
 * Recursive Lock Stubs
 * These functions are called by printf, malloc, etc., to
 * prevent race conditions in a multi-threaded environment.
 * Since minos-vi user processes are single-threaded,
 * we can define these as empty functions.
 */

void _lock_init_recursive(_lock_t *lock) {
    /* Do nothing */
}

void _lock_close_recursive(_lock_t *lock) {
    /* Do nothing */
}

void _lock_acquire_recursive(_lock_t *lock) {
    /* Do nothing */
}

void _lock_release_recursive(_lock_t *lock) {
    /* Do nothing */
}