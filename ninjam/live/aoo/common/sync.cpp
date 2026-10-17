/* Copyright (c) 2010-Now Christof Ressi, Winfried Ritsch and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include "sync.hpp"

#ifdef _WIN32
# include <windows.h>
#else
# include <pthread.h>
#endif

#include <cassert>

namespace aoo {
namespace sync {

//---------------------------- atomics -------------------------------------//

#if !defined(AOO_HAVE_ATOMIC_DOUBLE) || !defined(AOO_HAVE_ATOMIC_INT64)
namespace detail {
alignas(64) static spinlock g_atomic_spinlock;

void global_spinlock_lock() { g_atomic_spinlock.lock(); }

void global_spinlock_unlock() { g_atomic_spinlock.unlock(); }

}
#endif

//---------------------- mutex -------------------------//

#ifdef _WIN32

mutex::mutex() {
    InitializeSRWLock((PSRWLOCK)& mutex_);
}

mutex::~mutex() {}

void mutex::lock() {
    AcquireSRWLockExclusive((PSRWLOCK)&mutex_);
}

bool mutex::try_lock() {
    return TryAcquireSRWLockExclusive((PSRWLOCK)&mutex_);
}

void mutex::unlock() {
    ReleaseSRWLockExclusive((PSRWLOCK)&mutex_);
}

#else

mutex::mutex() {
    pthread_mutex_init(&mutex_, nullptr);
}

mutex::~mutex() {
    pthread_mutex_destroy(&mutex_);
}

void mutex::lock() {
    pthread_mutex_lock(&mutex_);
}

bool mutex::try_lock() {
    return pthread_mutex_trylock(&mutex_) == 0;
}

void mutex::unlock() {
    pthread_mutex_unlock(&mutex_);
}

#endif

//-------------------- shared_mutex -------------------------//

#if defined(_WIN32)

shared_mutex::shared_mutex() {
    InitializeSRWLock((PSRWLOCK)& rwlock_);
}

shared_mutex::~shared_mutex() {}

// exclusive
void shared_mutex::lock() {
    AcquireSRWLockExclusive((PSRWLOCK)&rwlock_);
}

bool shared_mutex::try_lock() {
    return TryAcquireSRWLockExclusive((PSRWLOCK)&rwlock_);
}

void shared_mutex::unlock() {
    ReleaseSRWLockExclusive((PSRWLOCK)&rwlock_);
}

// shared
void shared_mutex::lock_shared() {
    AcquireSRWLockShared((PSRWLOCK)&rwlock_);
}

bool shared_mutex::try_lock_shared() {
    return TryAcquireSRWLockShared((PSRWLOCK)&rwlock_);
}

void shared_mutex::unlock_shared() {
    ReleaseSRWLockShared((PSRWLOCK)&rwlock_);
}

#elif defined(AOO_HAVE_PTHREAD_RWLOCK)

shared_mutex::shared_mutex() {
    pthread_rwlock_init(&rwlock_, nullptr);
}

shared_mutex::~shared_mutex() {
    pthread_rwlock_destroy(&rwlock_);
}

// exclusive
void shared_mutex::lock() {
    pthread_rwlock_wrlock(&rwlock_);
}

bool shared_mutex::try_lock() {
    return pthread_rwlock_trywrlock(&rwlock_) == 0;
}
void shared_mutex::unlock() {
    pthread_rwlock_unlock(&rwlock_);
}

// shared
void shared_mutex::lock_shared() {
    pthread_rwlock_rdlock(&rwlock_);
}

bool shared_mutex::try_lock_shared() {
    return pthread_rwlock_tryrdlock(&rwlock_) == 0;
}

void shared_mutex::unlock_shared() {
    pthread_rwlock_unlock(&rwlock_);
}

#endif // _WIN32 || AOO_HAVE_PTHREAD_RWLOCK

} // sync
} // aoo
