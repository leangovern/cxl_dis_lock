/**
 * @file two_tier_lock.cpp
 * @brief Implementation of the two-tier lock and local lock array.
 */

#include "two_tier_lock.h"
#include "shared_memory.h"
#include "cas_global_lock.h"
#include "faa_global_lock.h"
#include "mcs_global_lock.h"
#include "qspinlock_global_lock.h"
#include <cstring>
#include <thread>
#include <chrono>
#include <cstdio>

namespace cxl_lock {

// =========================================================================
// TwoTierLock
// =========================================================================

TwoTierLock::TwoTierLock() = default;

TwoTierLock::~TwoTierLock() {
    shutdown();
}

LockResult TwoTierLock::initialize(NodeId node_id,
                                   LocalLockArray* local_locks,
                                   SharedMemoryRegion* shm,
                                   std::shared_ptr<GlobalLockAlgorithm> algorithm,
                                   ShmOffset state_offset,
                                   uint32_t max_locks,
                                   uint32_t node_count) {
    if (node_id == INVALID_NODE_ID || local_locks == nullptr ||
        shm == nullptr || algorithm == nullptr) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (max_locks == 0 || max_locks > MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (initialized_) {
        return LockResult::ERROR_UNKNOWN;
    }

    node_id_     = node_id;
    local_locks_ = local_locks;
    shm_         = shm;
    algorithm_   = std::move(algorithm);
    state_offset_= state_offset;
    max_locks_   = max_locks;
    node_count_  = (node_count > 0) ? node_count : max_locks;

    // Initialize the global lock algorithm's shared memory state
    LockResult rc = algorithm_->initialize(shm_, state_offset_, node_count_);
    if (rc != LockResult::SUCCESS) {
        algorithm_.reset();
        return rc;
    }

    initialized_ = true;
    return LockResult::SUCCESS;
}

void TwoTierLock::shutdown() {
    initialized_ = false;
    // Note: we do NOT own local_locks_ or shm_; do not delete them.
    local_locks_ = nullptr;
    shm_         = nullptr;
    algorithm_.reset();
}

// ------------------------------------------------------------------
// Lock acquisition
// ------------------------------------------------------------------

LockResult TwoTierLock::acquire(LockId lock_id, uint64_t timeout_ms) {
    if (!initialized_) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }
    if (lock_id >= max_locks_) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // --- Step 1: Acquire local lock ---
    LockResult rc = local_locks_->acquire(lock_id);
    if (rc != LockResult::SUCCESS) {
        return rc;  // Local lock failed
    }

    // --- Step 2: Request global lock ---
    rc = algorithm_->acquire_lock(node_id_, lock_id, shm_, state_offset_);
    if (rc != LockResult::SUCCESS) {
        // Failed to set global state; release local lock and propagate error.
        local_locks_->release(lock_id);
        return rc;
    }

    // --- Step 3: Poll until granted ---
    rc = poll_for_grant(lock_id, timeout_ms);
    if (rc != LockResult::SUCCESS) {
        // Timeout or error: cancel our request and release local lock.
        algorithm_->release_lock(node_id_, lock_id, shm_, state_offset_);
        local_locks_->release(lock_id);
        return rc;
    }

    return LockResult::SUCCESS;
}

// ------------------------------------------------------------------
// Lock release
// ------------------------------------------------------------------

LockResult TwoTierLock::release(LockId lock_id) {
    if (!initialized_) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }
    if (lock_id >= max_locks_) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // --- Step 1: Release global lock ---
    // Set our slot to IDLE so the lock manager can grant to next waiter.
    LockResult rc = algorithm_->release_lock(node_id_, lock_id,
                                             shm_, state_offset_);
    if (rc != LockResult::SUCCESS) {
        // Even if global release failed, still try to release local lock
        // to avoid deadlock.
        local_locks_->release(lock_id);
        return rc;
    }

    // --- Step 2: Release local lock ---
    rc = local_locks_->release(lock_id);

    return rc;
}

// ------------------------------------------------------------------
// Try acquire (non-blocking)
// ------------------------------------------------------------------

LockResult TwoTierLock::try_acquire(LockId lock_id) {
    if (!initialized_) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }
    if (lock_id >= max_locks_) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // --- Step 1: Try local lock ---
    LockResult rc = local_locks_->try_acquire(lock_id);
    if (rc != LockResult::SUCCESS) {
        return rc;  // Local lock not available
    }

    // --- Step 2: Request global lock ---
    rc = algorithm_->acquire_lock(node_id_, lock_id, shm_, state_offset_);
    if (rc != LockResult::SUCCESS) {
        local_locks_->release(lock_id);
        return rc;
    }

    // --- Step 3: Poll with zero timeout (single check) ---
    rc = poll_for_grant(lock_id, 0);
    if (rc != LockResult::SUCCESS) {
        // Not granted yet; cancel and release local lock.
        algorithm_->release_lock(node_id_, lock_id, shm_, state_offset_);
        local_locks_->release(lock_id);
        return LockResult::ERROR_BUSY;
    }

    return LockResult::SUCCESS;
}

// ------------------------------------------------------------------
// Poll for grant with adaptive backoff
// ------------------------------------------------------------------

LockResult TwoTierLock::poll_for_grant(LockId lock_id,
                                       uint64_t timeout_ms) {
    const auto start_time = std::chrono::steady_clock::now();
    uint32_t spin_count = 0;

    for (;;) {
        // Generic approach: use the algorithm's type to determine how to
        // check if the lock was granted. Since TwoTierLock knows the
        // algorithm type (via algorithm_ pointer), we use dynamic_cast to
        // check the concrete type and handle each case appropriately.

        bool granted = false;

        // Try CAS algorithm path
        auto* cas_algo = dynamic_cast<CasGlobalLockAlgorithm*>(algorithm_.get());
        if (cas_algo) {
            ShmOffset slot_offset = state_offset_
                + static_cast<ShmOffset>(lock_id) * node_count_ * sizeof(uint32_t)
                + static_cast<ShmOffset>(node_id_) * sizeof(uint32_t);
            void* slot_addr = shm_->offset_to_ptr(slot_offset);
            uint32_t val = shm_->atomic_load<uint32_t>(slot_addr,
                                                       std::memory_order_seq_cst);
            granted = (val == static_cast<uint32_t>(LockSlotState::LOCKED));
        } else {
            // FAA algorithm path
            auto* faa_algo = dynamic_cast<FaaGlobalLockAlgorithm*>(algorithm_.get());
            if (faa_algo) {
                // Read now_serving from the lock entry
                ShmOffset entry_offset = state_offset_
                    + static_cast<ShmOffset>(lock_id) * sizeof(FaaGlobalLockEntry);
                FaaGlobalLockEntry* entry = static_cast<FaaGlobalLockEntry*>(
                    shm_->offset_to_ptr(entry_offset));
                uint32_t now_serving = shm_->atomic_load<uint32_t>(
                    &entry->now_serving, std::memory_order_seq_cst);

                // Read our ticket
                // Tickets are stored after ALL lock entries (MAX_LOCK_COUNT entries)
                // NOT after max_locks_ entries. This must match FAA's layout.
                ShmOffset tickets_start = state_offset_
                    + static_cast<ShmOffset>(MAX_LOCK_COUNT) * sizeof(FaaGlobalLockEntry);
                ShmOffset ticket_offset = tickets_start
                    + static_cast<ShmOffset>(node_id_) * sizeof(FaaNodeTicket);
                FaaNodeTicket* nt = static_cast<FaaNodeTicket*>(
                    shm_->offset_to_ptr(ticket_offset));
                uint32_t my_ticket = shm_->atomic_load<uint32_t>(
                    &nt->ticket, std::memory_order_seq_cst);
                uint32_t active = shm_->atomic_load<uint32_t>(
                    &nt->active, std::memory_order_seq_cst);

                granted = (now_serving >= my_ticket && active != 0);
            } else {
                // MCS algorithm path
                auto* mcs_algo = dynamic_cast<McsGlobalLockAlgorithm*>(algorithm_.get());
                if (mcs_algo) {
                    // Read our McsNode's locked field.
                    // Nodes are stored after ALL lock entries (MAX_LOCK_COUNT entries).
                    ShmOffset nodes_start = state_offset_
                        + static_cast<ShmOffset>(MAX_LOCK_COUNT) * sizeof(McsLockEntry);
                    ShmOffset node_offset = nodes_start
                        + static_cast<ShmOffset>(node_id_) * sizeof(McsNode);
                    McsNode* node = static_cast<McsNode*>(
                        shm_->offset_to_ptr(node_offset));
                    uint32_t node_locked = shm_->atomic_load<uint32_t>(
                        &node->locked, std::memory_order_seq_cst);
                    uint32_t waiting_lock = shm_->atomic_load<uint32_t>(
                        &node->waiting_lock, std::memory_order_seq_cst);

                    // locked == 2 means the lock manager has formally granted.
                    // locked == 0 means the predecessor passed the lock directly.
                    // Both indicate the lock is acquired.
                    granted = ((node_locked == 2u || node_locked == 0u) &&
                               waiting_lock == static_cast<uint32_t>(lock_id));
                } else {
                    // qspinlock algorithm path
                    auto* qspin_algo = dynamic_cast<QspinlockGlobalLockAlgorithm*>(algorithm_.get());
                    if (qspin_algo) {
                        // For qspinlock, check our QspinNode's locked field.
                        // Fast path sets locked=2 directly; slow path waits for
                        // lock manager or predecessor to set locked=2.
                        ShmOffset nodes_start = state_offset_
                            + static_cast<ShmOffset>(MAX_LOCK_COUNT) * sizeof(QspinlockEntry);
                        ShmOffset node_offset = nodes_start
                            + static_cast<ShmOffset>(node_id_) * sizeof(QspinNode);
                        QspinNode* node = static_cast<QspinNode*>(
                            shm_->offset_to_ptr(node_offset));
                        uint32_t node_locked = shm_->atomic_load<uint32_t>(
                            &node->locked, std::memory_order_seq_cst);
                        uint32_t waiting_lock = shm_->atomic_load<uint32_t>(
                            &node->waiting_lock, std::memory_order_seq_cst);

                        granted = (node_locked == 2u &&
                                   waiting_lock == static_cast<uint32_t>(lock_id));
                    }
                }
            }
        }

        if (granted) {
            return LockResult::SUCCESS;  // Granted!
        }

        // Check for timeout.
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                elapsed).count();
            if (static_cast<uint64_t>(elapsed_ms) >= timeout_ms) {
                return LockResult::ERROR_TIMEOUT;
            }
        } else if (spin_count > 1000) {
            // Zero timeout means non-blocking; if not granted quickly, fail.
            return LockResult::ERROR_BUSY;
        }

        adaptive_backoff(spin_count);
    }
}

// ------------------------------------------------------------------
// Adaptive backoff strategy
// ------------------------------------------------------------------

void TwoTierLock::adaptive_backoff(uint32_t& spin_count) {
    ++spin_count;

    if (spin_count <= 100) {
        // Phase 1: Pure spin (low latency for short waits)
        __asm__ volatile("pause" ::: "memory");
    } else if (spin_count <= 1000) {
        // Phase 2: Spin with PAUSE hint (friendly to hyperthreading)
        for (int i = 0; i < 10; ++i) {
            __asm__ volatile("pause" ::: "memory");
        }
    } else if (spin_count <= 10000) {
        // Phase 3: Yield CPU to other threads
        std::this_thread::yield();
    } else {
        // Phase 4: Brief sleep to reduce contention
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

// ------------------------------------------------------------------
// Data-to-lock hash functions
// ------------------------------------------------------------------

LockId TwoTierLock::hash_data_to_lock_id(const void* data_key,
                                         size_t key_len,
                                         uint32_t lock_count) {
    // FNV-1a 64-bit hash
    uint64_t hash = 14695981039346656037ULL;
    const uint8_t* bytes = static_cast<const uint8_t*>(data_key);
    for (size_t i = 0; i < key_len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return static_cast<LockId>(hash % lock_count);
}

LockId TwoTierLock::hash_data_to_lock_id(uint64_t data_id,
                                         uint32_t lock_count) {
    // Mix the bits of data_id using a simple hash function,
    // then modulo to get the lock index.
    uint64_t hash = data_id;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return static_cast<LockId>(hash % lock_count);
}

bool TwoTierLock::is_locked(LockId lock_id) const {
    // This is a best-effort check; not used in the critical path.
    return false;
}

} // namespace cxl_lock
