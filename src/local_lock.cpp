/**
 * @file local_lock.cpp
 * @brief Implementation of the PIMPL-based LocalLockArray.
 *
 * This file provides the implementation for the LocalLockArray class defined
 * in local_lock.h using the PIMPL (Pointer to Implementation) idiom. The
 * internal implementation manages an array of pthread_mutex_t locks stored
 * in local DRAM (not CXL shared memory).
 */

#include "local_lock.h"
#include <cstring>
#include <new>
#include <cerrno>

namespace cxl_lock {

// ============================================================================
// LocalLockArray::Impl
// ============================================================================

class LocalLockArray::Impl {
public:
    Impl() = default;
    ~Impl() { destroy(); }

    // Non-copyable
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Movable
    Impl(Impl&& other) noexcept
        : mutexes_(other.mutexes_)
        , lock_count_(other.lock_count_)
        , initialized_(other.initialized_)
    {
        other.mutexes_ = nullptr;
        other.lock_count_ = 0;
        other.initialized_ = false;
    }

    Impl& operator=(Impl&& other) noexcept {
        if (this != &other) {
            destroy();
            mutexes_ = other.mutexes_;
            lock_count_ = other.lock_count_;
            initialized_ = other.initialized_;
            other.mutexes_ = nullptr;
            other.lock_count_ = 0;
            other.initialized_ = false;
        }
        return *this;
    }

    LockResult initialize(uint32_t lock_count) {
        if (lock_count == 0 || lock_count > MAX_LOCK_COUNT) {
            return LockResult::ERROR_INVALID_PARAM;
        }
        if (initialized_) {
            return LockResult::ERROR_UNKNOWN;
        }

        mutexes_ = new (std::nothrow) pthread_mutex_t[lock_count];
        if (mutexes_ == nullptr) {
            return LockResult::ERROR_SHM_FAILURE;
        }

        for (uint32_t i = 0; i < lock_count; ++i) {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            // Use error-checking mutex for debugging (can switch to
            // PTHREAD_MUTEX_NORMAL for production to reduce overhead).
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
            int rc = pthread_mutex_init(&mutexes_[i], &attr);
            pthread_mutexattr_destroy(&attr);
            if (rc != 0) {
                // Clean up already-initialized mutexes
                for (uint32_t j = 0; j < i; ++j) {
                    pthread_mutex_destroy(&mutexes_[j]);
                }
                delete[] mutexes_;
                mutexes_ = nullptr;
                return LockResult::ERROR_UNKNOWN;
            }
        }

        lock_count_ = lock_count;
        initialized_ = true;
        return LockResult::SUCCESS;
    }

    void destroy() {
        if (!initialized_ || mutexes_ == nullptr) {
            return;
        }

        for (uint32_t i = 0; i < lock_count_; ++i) {
            pthread_mutex_destroy(&mutexes_[i]);
        }

        delete[] mutexes_;
        mutexes_ = nullptr;
        lock_count_ = 0;
        initialized_ = false;
    }

    LockResult acquire(LockId lock_id) {
        if (!initialized_ || lock_id >= lock_count_) {
            return LockResult::ERROR_INVALID_PARAM;
        }
        int rc = pthread_mutex_lock(&mutexes_[lock_id]);
        return (rc == 0) ? LockResult::SUCCESS : LockResult::ERROR_UNKNOWN;
    }

    LockResult release(LockId lock_id) {
        if (!initialized_ || lock_id >= lock_count_) {
            return LockResult::ERROR_INVALID_PARAM;
        }
        int rc = pthread_mutex_unlock(&mutexes_[lock_id]);
        if (rc == 0) {
            return LockResult::SUCCESS;
        } else if (rc == EPERM) {
            return LockResult::ERROR_NOT_OWNER;
        }
        return LockResult::ERROR_UNKNOWN;
    }

    LockResult try_acquire(LockId lock_id) {
        if (!initialized_ || lock_id >= lock_count_) {
            return LockResult::ERROR_INVALID_PARAM;
        }
        int rc = pthread_mutex_trylock(&mutexes_[lock_id]);
        if (rc == 0) {
            return LockResult::SUCCESS;
        } else if (rc == EBUSY) {
            return LockResult::ERROR_BUSY;
        }
        return LockResult::ERROR_UNKNOWN;
    }

    bool is_held_by_this_thread(LockId lock_id) const {
        if (!initialized_ || lock_id >= lock_count_) {
            return false;
        }
        // pthread_mutex_t does not provide a standard way to check ownership
        // without trying to lock it. We use a non-blocking trylock to test.
        int rc = pthread_mutex_trylock(&mutexes_[lock_id]);
        if (rc == 0) {
            // We just acquired it, so it was NOT held by this thread.
            // Unlock it immediately.
            pthread_mutex_unlock(&mutexes_[lock_id]);
            return false;
        }
        // If EBUSY, someone holds it (might be us, might be another thread).
        // If EDEADLK or EPERM, the mutex type is error-checking and
        // indicates we already own it.
        if (rc == EDEADLK || rc == EPERM) {
            return true;
        }
        // For plain mutexes, EBUSY means owned by someone (could be us).
        // This is a best-effort check; plain mutexes don't track owners.
        return false;
    }

private:
    pthread_mutex_t* mutexes_ = nullptr;
    uint32_t         lock_count_ = 0;
    bool             initialized_ = false;
};

// ============================================================================
// LocalLockArray public interface (delegates to Impl)
// ============================================================================

LocalLockArray::LocalLockArray() : impl_(new Impl()) {}

LocalLockArray::~LocalLockArray() {
    delete impl_;
}

LocalLockArray::LocalLockArray(LocalLockArray&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

LocalLockArray& LocalLockArray::operator=(LocalLockArray&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

LockResult LocalLockArray::initialize(uint32_t lock_count) {
    return impl_->initialize(lock_count);
}

void LocalLockArray::destroy() {
    impl_->destroy();
}

LockResult LocalLockArray::acquire(LockId lock_id) {
    return impl_->acquire(lock_id);
}

LockResult LocalLockArray::release(LockId lock_id) {
    return impl_->release(lock_id);
}

LockResult LocalLockArray::try_acquire(LockId lock_id) {
    return impl_->try_acquire(lock_id);
}

bool LocalLockArray::is_held_by_this_thread(LockId lock_id) const {
    return impl_->is_held_by_this_thread(lock_id);
}

} // namespace cxl_lock
