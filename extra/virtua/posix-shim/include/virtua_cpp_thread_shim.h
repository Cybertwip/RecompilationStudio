#ifndef MVII_POSIX_SHIM_VIRTUA_CPP_THREAD_SHIM_H
#define MVII_POSIX_SHIM_VIRTUA_CPP_THREAD_SHIM_H

#if defined(__cplusplus) && defined(MINOS_VIRTUA_USERLAND)

// Announces that this header, and not libc++ and not Dash's <thread>, is the
// one defining std::this_thread in this translation unit. It defines it
// directly in namespace std; Dash's <thread> defines it in std::__1, which is
// an inline namespace, so both answer to the same `std::this_thread` and
// having both is not a redefinition but an ambiguity at every use site. Dash's
// header checks this macro and stands down. Same reason the x86_64 target does
// not force-include this file at all (see CMakeLists.txt): a second definition
// of a name libc++ already has is ambiguous, not additive.
#define MVII_POSIX_SHIM_HAS_STD_THIS_THREAD 1

#include <chrono>
#include <__type_traits/invoke.h>
#include <condition_variable>
#include <errno.h>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <tuple>
#include <type_traits>
#include <utility>

namespace std {

class mutex {
public:
    using native_handle_type = pthread_mutex_t*;

    mutex() noexcept {
        pthread_mutex_init(&mutex_, nullptr);
    }

    ~mutex() noexcept {
        pthread_mutex_destroy(&mutex_);
    }

    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock() {
        (void)pthread_mutex_lock(&mutex_);
    }

    bool try_lock() noexcept {
        return pthread_mutex_trylock(&mutex_) == 0;
    }

    void unlock() noexcept {
        (void)pthread_mutex_unlock(&mutex_);
    }

    native_handle_type native_handle() noexcept {
        return &mutex_;
    }

private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
};

class recursive_mutex {
public:
    using native_handle_type = pthread_mutex_t*;

    recursive_mutex() noexcept {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mutex_, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    ~recursive_mutex() noexcept {
        pthread_mutex_destroy(&mutex_);
    }

    recursive_mutex(const recursive_mutex&) = delete;
    recursive_mutex& operator=(const recursive_mutex&) = delete;

    void lock() {
        (void)pthread_mutex_lock(&mutex_);
    }

    bool try_lock() noexcept {
        return pthread_mutex_trylock(&mutex_) == 0;
    }

    void unlock() noexcept {
        (void)pthread_mutex_unlock(&mutex_);
    }

    native_handle_type native_handle() noexcept {
        return &mutex_;
    }

private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
};

enum class cv_status { no_timeout, timeout };

class condition_variable {
public:
    using native_handle_type = pthread_cond_t*;

    condition_variable() noexcept {
        pthread_cond_init(&cond_, nullptr);
    }

    ~condition_variable() noexcept {
        pthread_cond_destroy(&cond_);
    }

    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    void notify_one() noexcept {
        pthread_cond_signal(&cond_);
    }

    void notify_all() noexcept {
        pthread_cond_broadcast(&cond_);
    }

    void wait(unique_lock<mutex>& lock) {
        pthread_cond_wait(&cond_, lock.mutex()->native_handle());
    }

    template <class Predicate>
    void wait(unique_lock<mutex>& lock, Predicate pred) {
        while (!pred()) wait(lock);
    }

    template <class Rep, class Period>
    cv_status wait_for(unique_lock<mutex>& lock, const chrono::duration<Rep, Period>& rel_time) {
        using namespace chrono;
        const auto nanoseconds_to_wait = duration_cast<nanoseconds>(rel_time);
        if (nanoseconds_to_wait.count() <= 0) return cv_status::timeout;

        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        long long sec = nanoseconds_to_wait.count() / 1000000000LL;
        long nsec = static_cast<long>(nanoseconds_to_wait.count() % 1000000000LL);
        deadline.tv_sec += static_cast<time_t>(sec);
        deadline.tv_nsec += nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            ++deadline.tv_sec;
        }

        const int rc = pthread_cond_timedwait(&cond_, lock.mutex()->native_handle(), &deadline);
        return rc == ETIMEDOUT ? cv_status::timeout : cv_status::no_timeout;
    }

    template <class Rep, class Period, class Predicate>
    bool wait_for(unique_lock<mutex>& lock, const chrono::duration<Rep, Period>& rel_time, Predicate pred) {
        if (pred()) return true;
        return wait_for(lock, rel_time) == cv_status::no_timeout && pred();
    }

    template <class Clock, class Duration>
    cv_status wait_until(unique_lock<mutex>& lock, const chrono::time_point<Clock, Duration>& abs_time) {
        return wait_for(lock, abs_time - Clock::now());
    }

    template <class Clock, class Duration, class Predicate>
    bool wait_until(unique_lock<mutex>& lock, const chrono::time_point<Clock, Duration>& abs_time, Predicate pred) {
        while (!pred()) {
            if (wait_until(lock, abs_time) == cv_status::timeout) return pred();
        }
        return true;
    }

    native_handle_type native_handle() noexcept {
        return &cond_;
    }

private:
    pthread_cond_t cond_ = PTHREAD_COND_INITIALIZER;
};

class condition_variable_any {
public:
    condition_variable_any() = default;
    ~condition_variable_any() = default;

    condition_variable_any(const condition_variable_any&) = delete;
    condition_variable_any& operator=(const condition_variable_any&) = delete;

    void notify_one() noexcept {}
    void notify_all() noexcept {}

    template <class Lock>
    void wait(Lock& lock) {
        lock.unlock();
        sched_yield();
        lock.lock();
    }

    template <class Lock, class Predicate>
    void wait(Lock& lock, Predicate pred) {
        while (!pred()) wait(lock);
    }

    template <class Lock, class Rep, class Period>
    cv_status wait_for(Lock& lock, const chrono::duration<Rep, Period>&) {
        wait(lock);
        return cv_status::no_timeout;
    }

    template <class Lock, class Rep, class Period, class Predicate>
    bool wait_for(Lock& lock, const chrono::duration<Rep, Period>& rel_time, Predicate pred) {
        if (pred()) return true;
        (void)wait_for(lock, rel_time);
        return pred();
    }
};

class thread {
public:
    class id {
    public:
        id() noexcept = default;

    private:
        explicit id(pthread_t value) noexcept : value_(value) {}

        pthread_t value_ = 0;

        friend class thread;
        friend struct hash<thread::id>;
        friend bool operator==(id lhs, id rhs) noexcept;
        friend bool operator<(id lhs, id rhs) noexcept;
        friend id this_thread_get_id() noexcept;
    };

    using native_handle_type = pthread_t;

    thread() noexcept = default;

    template <class F,
              class... Args,
              class = typename enable_if<!is_same<typename remove_cv<typename remove_reference<F>::type>::type,
                                                   thread>::value>::type>
    explicit thread(F&& f, Args&&... args) {
        using State = ThreadState<F, Args...>;
        auto* state = new State(std::forward<F>(f), std::forward<Args>(args)...);
        if (pthread_create(&thread_, nullptr, &State::run, state) == 0) {
            joinable_ = true;
        } else {
            delete state;
            thread_ = 0;
        }
    }

    ~thread() {
        if (joinable_) detach();
    }

    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    thread(thread&& other) noexcept {
        swap(other);
    }

    thread& operator=(thread&& other) noexcept {
        if (this != &other) {
            if (joinable_) detach();
            thread_ = other.thread_;
            joinable_ = other.joinable_;
            other.thread_ = 0;
            other.joinable_ = false;
        }
        return *this;
    }

    void swap(thread& other) noexcept {
        std::swap(thread_, other.thread_);
        std::swap(joinable_, other.joinable_);
    }

    bool joinable() const noexcept {
        return joinable_;
    }

    void join() {
        if (!joinable_) return;
        pthread_join(thread_, nullptr);
        thread_ = 0;
        joinable_ = false;
    }

    void detach() {
        if (!joinable_) return;
        pthread_detach(thread_);
        thread_ = 0;
        joinable_ = false;
    }

    id get_id() const noexcept {
        return joinable_ ? id(thread_) : id();
    }

    native_handle_type native_handle() noexcept {
        return thread_;
    }

    static unsigned hardware_concurrency() noexcept {
        return 1;
    }

private:
    template <size_t... Indices>
    struct IndexSequence {};

    template <size_t Count, size_t... Indices>
    struct MakeIndexSequence : MakeIndexSequence<Count - 1, Count - 1, Indices...> {};

    template <size_t... Indices>
    struct MakeIndexSequence<0, Indices...> {
        using Type = IndexSequence<Indices...>;
    };

    template <class F, class... Args>
    struct ThreadState {
        using Tuple = tuple<typename decay<F>::type, typename decay<Args>::type...>;

        explicit ThreadState(F&& f, Args&&... args)
            : work(std::forward<F>(f), std::forward<Args>(args)...) {}

        template <size_t... Indices>
        static void invoke(Tuple& tuple, IndexSequence<Indices...>) {
            std::__invoke(std::get<0>(tuple), std::get<Indices + 1>(tuple)...);
        }

        static void* run(void* raw) noexcept {
            auto* state = static_cast<ThreadState*>(raw);
            try {
                invoke(state->work, typename MakeIndexSequence<sizeof...(Args)>::Type());
            } catch (...) {
                std::terminate();
            }
            delete state;
            return nullptr;
        }

        Tuple work;
    };

    pthread_t thread_ = 0;
    bool joinable_ = false;
};

inline bool operator==(thread::id lhs, thread::id rhs) noexcept {
    return lhs.value_ == rhs.value_;
}

inline bool operator!=(thread::id lhs, thread::id rhs) noexcept {
    return !(lhs == rhs);
}

inline bool operator<(thread::id lhs, thread::id rhs) noexcept {
    return lhs.value_ < rhs.value_;
}

inline void swap(thread& lhs, thread& rhs) noexcept {
    lhs.swap(rhs);
}

inline thread::id this_thread_get_id() noexcept {
    return thread::id(pthread_self());
}

template <>
struct hash<thread::id> {
    size_t operator()(thread::id value) const noexcept {
        return static_cast<size_t>(value.value_);
    }
};

namespace this_thread {

inline thread::id get_id() noexcept {
    return this_thread_get_id();
}

inline void yield() noexcept {
    sched_yield();
}

template <class Rep, class Period>
void sleep_for(const chrono::duration<Rep, Period>& rel_time) {
    using namespace chrono;
    const auto nanoseconds_to_sleep = duration_cast<nanoseconds>(rel_time);
    if (nanoseconds_to_sleep.count() <= 0) return;
    timespec req{};
    req.tv_sec = static_cast<time_t>(nanoseconds_to_sleep.count() / 1000000000LL);
    req.tv_nsec = static_cast<long>(nanoseconds_to_sleep.count() % 1000000000LL);
    nanosleep(&req, nullptr);
}

template <class Clock, class Duration>
void sleep_until(const chrono::time_point<Clock, Duration>& abs_time) {
    sleep_for(abs_time - Clock::now());
}

} // namespace this_thread

inline void notify_all_at_thread_exit(condition_variable& cond, unique_lock<mutex>) {
    cond.notify_all();
}

enum class future_status { ready, timeout, deferred };
enum class launch { async = 1, deferred = 2, any = 3 };

inline launch operator|(launch lhs, launch rhs) {
    return static_cast<launch>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

inline bool virtua_launch_has(launch policy, launch bit) {
    return (static_cast<int>(policy) & static_cast<int>(bit)) != 0;
}

template <class T>
struct VirtuaFutureState {
    mutable mutex lock;
    mutable condition_variable ready_condition;
    bool ready = false;
    unique_ptr<T> value;
    exception_ptr exception;
};

template <>
struct VirtuaFutureState<void> {
    mutable mutex lock;
    mutable condition_variable ready_condition;
    bool ready = false;
    exception_ptr exception;
};

template <class T>
class future;

template <class T>
class promise {
public:
    promise() : state_(make_shared<VirtuaFutureState<T>>()) {}
    promise(promise&&) noexcept = default;
    promise& operator=(promise&&) noexcept = default;

    promise(const promise&) = delete;
    promise& operator=(const promise&) = delete;

    future<T> get_future();

    void set_value(const T& value) {
        unique_lock<mutex> guard(state_->lock);
        state_->value.reset(new T(value));
        state_->ready = true;
        guard.unlock();
        state_->ready_condition.notify_all();
    }

    void set_value(T&& value) {
        unique_lock<mutex> guard(state_->lock);
        state_->value.reset(new T(std::move(value)));
        state_->ready = true;
        guard.unlock();
        state_->ready_condition.notify_all();
    }

    void set_exception(exception_ptr exception) {
        unique_lock<mutex> guard(state_->lock);
        state_->exception = exception;
        state_->ready = true;
        guard.unlock();
        state_->ready_condition.notify_all();
    }

    void set_value_at_thread_exit(const T& value) { set_value(value); }
    void set_value_at_thread_exit(T&& value) { set_value(std::move(value)); }
    void set_exception_at_thread_exit(exception_ptr exception) { set_exception(exception); }

private:
    shared_ptr<VirtuaFutureState<T>> state_;

    friend class future<T>;
};

template <>
class promise<void> {
public:
    promise() : state_(make_shared<VirtuaFutureState<void>>()) {}
    promise(promise&&) noexcept = default;
    promise& operator=(promise&&) noexcept = default;

    promise(const promise&) = delete;
    promise& operator=(const promise&) = delete;

    future<void> get_future();

    void set_value() {
        unique_lock<mutex> guard(state_->lock);
        state_->ready = true;
        guard.unlock();
        state_->ready_condition.notify_all();
    }

    void set_exception(exception_ptr exception) {
        unique_lock<mutex> guard(state_->lock);
        state_->exception = exception;
        state_->ready = true;
        guard.unlock();
        state_->ready_condition.notify_all();
    }

    void set_value_at_thread_exit() { set_value(); }
    void set_exception_at_thread_exit(exception_ptr exception) { set_exception(exception); }

private:
    shared_ptr<VirtuaFutureState<void>> state_;

    friend class future<void>;
};

template <class T>
class future {
public:
    future() noexcept = default;
    future(future&&) noexcept = default;
    future& operator=(future&&) noexcept = default;

    future(const future&) = delete;
    future& operator=(const future&) = delete;

    T get() {
        wait();
        if (state_->exception) rethrow_exception(state_->exception);
        return std::move(*state_->value);
    }

    bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    void wait() const {
        if (!state_) return;
        unique_lock<mutex> guard(state_->lock);
        state_->ready_condition.wait(guard, [this] { return state_->ready; });
    }

    template <class Rep, class Period>
    future_status wait_for(const chrono::duration<Rep, Period>& rel_time) const {
        if (!state_) return future_status::ready;
        unique_lock<mutex> guard(state_->lock);
        return state_->ready_condition.wait_for(guard, rel_time, [this] { return state_->ready; })
            ? future_status::ready
            : future_status::timeout;
    }

    template <class Clock, class Duration>
    future_status wait_until(const chrono::time_point<Clock, Duration>& abs_time) const {
        return wait_for(abs_time - Clock::now());
    }

private:
    explicit future(shared_ptr<VirtuaFutureState<T>> state) : state_(std::move(state)) {}

    shared_ptr<VirtuaFutureState<T>> state_;

    friend class promise<T>;
};

template <>
class future<void> {
public:
    future() noexcept = default;
    future(future&&) noexcept = default;
    future& operator=(future&&) noexcept = default;

    future(const future&) = delete;
    future& operator=(const future&) = delete;

    void get() {
        wait();
        if (state_ && state_->exception) rethrow_exception(state_->exception);
    }

    bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    void wait() const {
        if (!state_) return;
        unique_lock<mutex> guard(state_->lock);
        state_->ready_condition.wait(guard, [this] { return state_->ready; });
    }

    template <class Rep, class Period>
    future_status wait_for(const chrono::duration<Rep, Period>& rel_time) const {
        if (!state_) return future_status::ready;
        unique_lock<mutex> guard(state_->lock);
        return state_->ready_condition.wait_for(guard, rel_time, [this] { return state_->ready; })
            ? future_status::ready
            : future_status::timeout;
    }

    template <class Clock, class Duration>
    future_status wait_until(const chrono::time_point<Clock, Duration>& abs_time) const {
        return wait_for(abs_time - Clock::now());
    }

private:
    explicit future(shared_ptr<VirtuaFutureState<void>> state) : state_(std::move(state)) {}

    shared_ptr<VirtuaFutureState<void>> state_;

    friend class promise<void>;
};

template <class T>
future<T> promise<T>::get_future() {
    return future<T>(state_);
}

inline future<void> promise<void>::get_future() {
    return future<void>(state_);
}

template <class Signature>
class packaged_task;

template <class R, class... Args>
class VirtuaPackagedCallableBase {
public:
    virtual ~VirtuaPackagedCallableBase() = default;
    virtual R call(Args... args) = 0;
};

template <class F, class R, class... Args>
class VirtuaPackagedCallable final : public VirtuaPackagedCallableBase<R, Args...> {
public:
    template <class Fn>
    explicit VirtuaPackagedCallable(Fn&& fn) : callable_(std::forward<Fn>(fn)) {}

    R call(Args... args) override {
        return std::__invoke(callable_, std::forward<Args>(args)...);
    }

private:
    F callable_;
};

template <class F, class... Args>
class VirtuaPackagedCallable<F, void, Args...> final : public VirtuaPackagedCallableBase<void, Args...> {
public:
    template <class Fn>
    explicit VirtuaPackagedCallable(Fn&& fn) : callable_(std::forward<Fn>(fn)) {}

    void call(Args... args) override {
        std::__invoke(callable_, std::forward<Args>(args)...);
    }

private:
    F callable_;
};

template <class R, class... Args>
class packaged_task<R(Args...)> {
public:
    packaged_task() = default;

    template <class F>
    explicit packaged_task(F&& f)
        : callable_(new VirtuaPackagedCallable<typename decay<F>::type, R, Args...>(std::forward<F>(f))) {}

    packaged_task(packaged_task&&) noexcept = default;
    packaged_task& operator=(packaged_task&&) noexcept = default;

    packaged_task(const packaged_task&) = delete;
    packaged_task& operator=(const packaged_task&) = delete;

    future<R> get_future() {
        return promise_.get_future();
    }

    void operator()(Args... args) {
        run(typename is_void<R>::type(), std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(callable_);
    }

private:
    void run(false_type, Args... args) {
        try {
            promise_.set_value(callable_->call(std::forward<Args>(args)...));
        } catch (...) {
            promise_.set_exception(current_exception());
        }
    }

    void run(true_type, Args... args) {
        try {
            callable_->call(std::forward<Args>(args)...);
            promise_.set_value();
        } catch (...) {
            promise_.set_exception(current_exception());
        }
    }

    unique_ptr<VirtuaPackagedCallableBase<R, Args...>> callable_;
    promise<R> promise_;
};

template <class F, class... Args>
auto async(launch policy, F&& f, Args&&... args)
    -> future<decltype(std::__invoke(std::declval<F>(), std::declval<Args>()...))> {
    using R = decltype(std::__invoke(std::declval<F>(), std::declval<Args>()...));
    typedef packaged_task<R()> Task;
    auto task = make_shared<Task>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    auto result = task->get_future();
    if (virtua_launch_has(policy, launch::async) || !virtua_launch_has(policy, launch::deferred)) {
        thread([task] { (*task)(); }).detach();
    } else {
        (*task)();
    }
    return result;
}

template <class F, class... Args>
auto async(F&& f, Args&&... args)
    -> future<decltype(std::__invoke(std::declval<F>(), std::declval<Args>()...))> {
    return std::async(launch::async, std::forward<F>(f), std::forward<Args>(args)...);
}

} // namespace std

#endif

#endif
