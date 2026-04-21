/**
 * @file two_tier_lock.h
 * @brief Two-tier lock combining local DRAM mutexes with global CXL locks.
 *
 * The TwoTierLock is the main user-facing API for acquiring distributed locks.
 * It combines:
 *   - Local tier:  Per-node pthread_mutex_t locks in local DRAM (fast, uncontended)
 *   - Global tier: Lock entries in CXL shared memory (coordinator across nodes)
 *
 * Lock acquisition flow:
 *   1. Acquire local lock (pthread_mutex_lock)
 *   2. Set global slot to WAITING (via GlobalLockAlgorithm)
 *   3. Poll global slot until it becomes LOCKED
 *
 * Lock release flow:
 *   1. Set global slot to IDLE (via GlobalLockAlgorithm)
 *   2. Release local lock (pthread_mutex_unlock)
 */

#pragma once

#include "lock_interface.h"
#include "local_lock.h"
#include "cas_global_lock.h"
#include "faa_global_lock.h"
#include "mcs_global_lock.h"
#include "qspinlock_global_lock.h"
#include <pthread.h>
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <memory>

namespace cxl_lock {

// Forward declarations
class CxlMemoryPool;
class GlobalLockAlgorithm;

// ---------------------------------------------------------------------------
// TwoTierLock
// ---------------------------------------------------------------------------

/**
 * @brief Two-tier distributed lock for CXL shared memory.
 *
 * This is the primary interface used by application code to acquire and
 * release distributed locks. It coordinates the local DRAM mutex with the
 * global CXL lock state.
 */
class TwoTierLock {
public:
    TwoTierLock();
    ~TwoTierLock();

    // Non-copyable, non-movable
    TwoTierLock(const TwoTierLock&) = delete;
    TwoTierLock& operator=(const TwoTierLock&) = delete;
    TwoTierLock(TwoTierLock&&) = delete;
    TwoTierLock& operator=(TwoTierLock&&) = delete;

    /**
     * @brief Initialize this node's two-tier lock client.
     *
     * @param node_id       This node's ID
     * @param local_locks   Local DRAM mutex array (owned externally)
     * @param shm           CXL shared memory region
     * @param algorithm     Global lock algorithm instance
     * @param state_offset  Offset of global lock state in shared memory
     * @param max_locks     Maximum number of locks
     * @return LockResult::SUCCESS on success
     */
    LockResult initialize(NodeId node_id,
                          LocalLockArray* local_locks,
                          SharedMemoryRegion* shm,
                          std::shared_ptr<GlobalLockAlgorithm> algorithm,
                          ShmOffset state_offset,
                          uint32_t max_locks,
                          uint32_t node_count = 0);

    /// Shutdown and release resources.
    void shutdown();

    /**
     * @brief Acquire a lock (blocking, with optional timeout).
     *
     * Steps:
     *   1. Acquire local lock
     *   2. Set global state to WAITING
     *   3. Poll global state until LOCKED (or timeout)
     *
     * @param lock_id     Lock to acquire
     * @param timeout_ms  Timeout in milliseconds (0 = no timeout)
     * @return LockResult::SUCCESS if lock acquired
     */
    LockResult acquire(LockId lock_id, uint64_t timeout_ms = 0);

    /**
     * @brief Release a lock.
     *
     * Steps:
     *   1. Set global state to IDLE
     *   2. Release local lock
     *
     * @param lock_id  Lock to release
     * @return LockResult::SUCCESS if lock released
     */
    LockResult release(LockId lock_id);

    /**
     * @brief Try to acquire a lock (non-blocking).
     *
     * @param lock_id  Lock to try acquiring
     * @return LockResult::SUCCESS if acquired, ERROR_BUSY if not available
     */
    LockResult try_acquire(LockId lock_id);

    /// Check if this node currently holds a given lock.
    bool is_locked(LockId lock_id) const;

    // ------------------------------------------------------------------
    // Data-to-lock mapping helpers
    // ------------------------------------------------------------------

    /**
     * @brief Hash arbitrary data to a lock ID using FNV-1a.
     * @return Lock ID in range [0, lock_count)
     */
    static LockId hash_data_to_lock_id(const void* data_key,
                                       size_t key_len,
                                       uint32_t lock_count);

    /**
     * @brief Hash a 64-bit data ID to a lock ID.
     * @return Lock ID in range [0, lock_count)
     */
    static LockId hash_data_to_lock_id(uint64_t data_id, uint32_t lock_count);

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Poll the global lock state until LOCKED or timeout.
    LockResult poll_for_grant(LockId lock_id, uint64_t timeout_ms);

    /// Adaptive backoff: spin -> yield -> sleep.
    static void adaptive_backoff(uint32_t& spin_count);

    // ------------------------------------------------------------------
    // Member data
    // ------------------------------------------------------------------

    NodeId               node_id_ = INVALID_NODE_ID;
    LocalLockArray*      local_locks_ = nullptr;
    SharedMemoryRegion*  shm_ = nullptr;
    std::shared_ptr<GlobalLockAlgorithm> algorithm_;
    ShmOffset            state_offset_ = 0;
    uint32_t             max_locks_ = 0;
    uint32_t             node_count_ = 0;  // Number of nodes (for offset calculations)
    bool                 initialized_ = false;
};

} // namespace cxl_lock
