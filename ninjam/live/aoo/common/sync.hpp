/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#pragma once

#include <inttypes.h>
#include <atomic>
#include <thread>
#include <utility>

#ifndef _WIN32
  #include <pthread.h>
#endif


// for spinlock
#if defined(_MSC_VER)
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <Windows.h>
#elif defined(__SSE2__)
# include <immintrin.h>
#endif

// for shared_lock
#include <mutex>
#include <shared_mutex>

namespace aoo {
namespace sync {

inline void pause_cpu() {
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__SSE2__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("isb");
#elif defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(ESP_PLATFORM)
    // TODO
#else
# warning "Cannot pause CPU, falling back to busy-waiting."
#endif
}

//----------------- relaxed atomics ---------------//

namespace detail {
#if !defined(AOO_HAVE_ATOMIC_DOUBLE) || !defined(AOO_HAVE_ATOMIC_INT64)
// emulate atomics using a global spinlock
void global_spinlock_lock();
void global_spinlock_unlock();

template<typename T>
class relaxed_atomic_emulated {
private:
    struct scoped_lock {
        scoped_lock() { global_spinlock_lock(); }
        ~scoped_lock() { global_spinlock_unlock(); }
    };
public:
    relaxed_atomic_emulated(T value = T{}) : value_(value) {}

    T load() const {
        scoped_lock lock;
        return value_;
    }

    void store(T value) {
        scoped_lock lock;
        value_ = value;
    }

    T exchange(T value) {
        scoped_lock lock;
        return std::exchange(value_, value);
    }
private:
    T value_;
};
#endif

// native atomics
template<typename T>
class relaxed_atomic_native {
public:
    relaxed_atomic_native(T value = T{}) : value_(value) {}

    T load() const {
        return value_.load(std::memory_order_relaxed);
    }

    void store(T value) {
        value_.store(value, std::memory_order_relaxed);
    }

    T exchange(T value) {
        return value_.exchange(value, std::memory_order_relaxed);
    }
private:
#if __cplusplus >= 201703L && !defined(ESP_PLATFORM)
    // The Xtensa does not guarantee lockfree atomics for any data type.
    // E.g. __GCC_ATOMIC_INT_LOCK_FREE evaluates to 1 (= sometimes lockfree).
    // In practice all types up to 4 bytes should be lockfree on an ESP32 chip.
    // Ideally we should do a runtime check with std::atomic::is_lock_free(),
    // but that wouldn't work for cross-compiling...
    static_assert(std::atomic<T>::is_always_lock_free,
                  "std::atomic<T> is not lockfree!");
#endif
    std::atomic<T> value_;
};

} // detail

template<typename T, typename enable = void>
class relaxed_atomic
        : public detail::relaxed_atomic_native<T>
{
    using detail::relaxed_atomic_native<T>::relaxed_atomic_native;
};

// specialization for doubles (if needed)
#ifndef AOO_HAVE_ATOMIC_DOUBLE
// #warning "emulating atomic doubles in software"
template<>
class relaxed_atomic<double>
        : public detail::relaxed_atomic_emulated<double>
{
    using relaxed_atomic_emulated::relaxed_atomic_emulated;
};
#endif

// specialization for 64-bit integers (if needed)
#ifndef AOO_HAVE_ATOMIC_INT64
// #warning "emulating atomic 64-bit integers in software"
template<typename T>
class relaxed_atomic<T, typename std::enable_if<
        std::is_integral<T>::value && (sizeof(T) == 8)>::type>
        : public detail::relaxed_atomic_emulated<T>
{
    using detail::relaxed_atomic_emulated<T>::relaxed_atomic_emulated;
};
#endif


//----------------- spinlock ----------------------//

// TODO: temporarily disable interrupts on ESP?
class spinlock {
public:
    spinlock() = default;
    spinlock(const spinlock&) = delete;
    spinlock& operator=(const spinlock&) = delete;

    void lock() {
        // only try to modify the shared state if the lock seems to be available.
        // this should prevent unnecessary cache invalidation.
        do {
            while (locked_.load(std::memory_order_relaxed)){
                pause_cpu();
            }
        } while (!try_lock());
    }

    bool try_lock() {
        // if the previous value was false, it means be could aquire the lock.
        // this is faster than a CAS loop.
        return !locked_.exchange(true, std::memory_order_acquire);
    }

    void unlock() {
        locked_.store(false, std::memory_order_release);
    }
protected:
    std::atomic<uint32_t> locked_{false};
};

//------------------------------ mutex ------------------------------------//

// The std::mutex implementation on Windows rolls their own mutex based on atomics and
// Events, but for our cases we can just use the most suitable platform primitive (SRWLOCK).
//
// Older OSX versions (OSX 10.11 and below) don't have std:shared_mutex...
//
// Even on Linux, there's some overhead for things we don't need, so we use pthreads directly.

class mutex {
public:
    mutex();
    ~mutex();
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    void lock();
    bool try_lock();
    void unlock();
private:
#ifdef _WIN32
    void* mutex_; // avoid including windows headers (SWRLOCK is pointer sized)
#else
    pthread_mutex_t mutex_;
#endif
};

//------------------------ shared_mutex -------------------------//

#if defined(_WIN32) || defined(AOO_HAVE_PTHREAD_RWLOCK)

class shared_mutex {
public:
    shared_mutex();
    ~shared_mutex();
    shared_mutex(const shared_mutex&) = delete;
    shared_mutex& operator=(const shared_mutex&) = delete;
    // exclusive
    void lock();
    bool try_lock();
    void unlock();
    // shared
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared();
private:
#ifdef _WIN32
    void* rwlock_; // avoid including windows headers (SWRLOCK is pointer sized)
#else
    pthread_rwlock_t rwlock_;
#endif
};

#else

// fallback
using shared_mutex = std::shared_mutex;

#endif // _WIN32 || AOO_HAVE_PTHREAD_RWLOCK

//----------------------- lock guards ------------------------------//

typedef std::try_to_lock_t try_to_lock_t;
typedef std::defer_lock_t defer_lock_t;
typedef std::adopt_lock_t adopt_lock_t;

constexpr try_to_lock_t try_to_lock {};
constexpr defer_lock_t defer_lock {};
constexpr adopt_lock_t adopt_lock {};

template<typename T>
using shared_lock = std::shared_lock<T>;

template<typename T>
using unique_lock = std::unique_lock<T>;

template<typename T>
class scoped_lock {
public:
    scoped_lock(T& lock)
        : lock_(lock){ lock_.lock(); }
    scoped_lock(const scoped_lock& lock) = delete;
    scoped_lock& operator=(const scoped_lock& lock) = delete;
    ~scoped_lock() { lock_.unlock(); }
private:
    T& lock_;
};

template<typename T>
class scoped_shared_lock {
public:
    scoped_shared_lock(T& lock)
        : lock_(lock){ lock_.lock_shared(); }
    scoped_shared_lock(const scoped_shared_lock& lock) = delete;
    scoped_shared_lock& operator=(const scoped_shared_lock& lock) = delete;
    ~scoped_shared_lock() { lock_.unlock_shared(); }
private:
    T& lock_;
};

} // sync
} // aoo
