/**
 * @file local_lock.h
 * @brief Local DRAM-resident mutex lock array for per-node locking.
 *
 * This header provides the LocalLockArray class, which manages an array of
 * pthread mutex locks stored in local DRAM (NOT in CXL shared memory). Each
 * node in the cluster has its own LocalLockArray instance.
 *
 * The local lock serves as the first tier of the two-tier locking mechanism:
 *   1. A thread first acquires its node's local mutex for the given lock ID.
 *   2. Only if the node does not already hold the corresponding global lock
 *      does the thread proceed to the global lock algorithm.
 *   3. This local-first design dramatically reduces contention on the CXL
 *      shared memory fabric, since multiple threads on the same node can
 *      share a single global lock grant.
 *
 * The local lock array maps each LockId (0 to MAX_LOCK_COUNT-1) to a
 * pthread_mutex_t. The array is indexed directly by lock_id for O(1) access.
 *
 * Thread Safety: Each mutex in the array can be locked independently.
 * The array itself is read-only after initialization and is safe for
 * concurrent lock/unlock operations on different lock IDs.
 *
 * Performance Considerations:
 *   - The array is sized for MAX_LOCK_COUNT (1024) entries.
 *   - Each entry is a pthread_mutex_t, typically 40-56 bytes.
 *   - Total memory footprint: ~40-56 KB per node, all in local DRAM.
 *   - Lock acquisition is a local pthread_mutex_lock() with no CXL traffic.
 */
#pragma once

#include "lock_types.h"
#include <pthread.h>

namespace cxl_lock {

// ---------------------------------------------------------------------------
// LocalLockArray
// ---------------------------------------------------------------------------

/**
 * @brief Per-node array of local DRAM-resident mutex locks.
 *
 * The LocalLockArray manages a fixed-size array of pthread_mutex_t locks,
 * one for each possible LockId in the system. These locks are stored in
 * local DRAM and are NOT shared across nodes - each node has its own
 * independent copy.
 *
 * Role in the Two-Tier Locking Mechanism:
 * @code
 *   // Thread wants to acquire lock for LockId=42:
 *
 *   // Tier 1: Local lock (DRAM, node-local)
 *   local_lock_array.acquire(lock_id);  // pthread_mutex_lock()
 *
 *   // Check if this node already holds the global lock for this lock_id
 *   if (!node_already_holds_global_lock(lock_id)) {
 *       // Tier 2: Global lock (CXL shared memory, cross-node)
 *       global_algorithm->acquire_lock(node_id, lock_id, shm, offset);
 *       // ... wait for lock manager to grant ...
 *   }
 *
 *   // Critical section: access shared data in CXL memory
 *   // ...
 *
 *   // Release in reverse order
 *   global_algorithm->release_lock(node_id, lock_id, shm, offset);
 *   local_lock_array.release(lock_id);  // pthread_mutex_unlock()
 * @endcode
 *
 * By acquiring the local lock first, we ensure that only one thread per
 * node can be in the process of acquiring/releasing the global lock at
 * any given time. This amortizes the cost of the global lock operation
 * across all threads on the node that want the same lock.
 *
 * The PIMPL idiom is used to hide the internal array storage and
 * pthread-specific details from the header.
 */
class LocalLockArray {
public:
    /**
     * @brief Construct an uninitialized LocalLockArray.
     *
     * The array is not usable until initialize() is called.
     */
    LocalLockArray();

    /**
     * @brief Destructor - destroys all mutex locks in the array.
     *
     * If the array was initialized, destroy() is called automatically.
     * Any threads holding locks at destruction time will encounter
     * undefined behavior (same as destroying a locked pthread_mutex_t).
     */
    ~LocalLockArray();

    // Disable copy; local lock arrays are per-node resources.
    LocalLockArray(const LocalLockArray&) = delete;
    LocalLockArray& operator=(const LocalLockArray&) = delete;

    // Enable move semantics for transfer of ownership.
    LocalLockArray(LocalLockArray&& other) noexcept;
    LocalLockArray& operator=(LocalLockArray&& other) noexcept;

    /**
     * @brief Initialize the local lock array.
     *
     * Allocates and initializes (pthread_mutex_init) an array of
     * pthread_mutex_t locks. The number of locks must not exceed
     * MAX_LOCK_COUNT.
     *
     * The mutexes are created with default attributes (plain mutexes,
     * not recursive). Error checking mutexes can be enabled via compile-time
     * configuration for debugging purposes.
     *
     * @param lock_count Number of locks to create (1 to MAX_LOCK_COUNT).
     * @return LockResult::SUCCESS if all locks were initialized.
     *         ERROR_INVALID_PARAM if lock_count is 0 or exceeds MAX_LOCK_COUNT.
     *         ERROR_SHM_FAILURE if memory allocation failed.
     */
    LockResult initialize(uint32_t lock_count);

    /**
     * @brief Destroy all mutex locks in the array.
     *
     * Calls pthread_mutex_destroy on each lock and frees the array memory.
     * After destruction, the array must be re-initialized before use.
     *
     * This is called automatically by the destructor but can also be called
     * explicitly for controlled cleanup.
     */
    void destroy();

    /**
     * @brief Acquire a local lock by lock ID.
     *
     * Blocks until the lock is available, then acquires it for the calling
     * thread. This is a wrapper around pthread_mutex_lock().
     *
     * This is the first step in the two-tier lock acquisition. The calling
     * thread must hold this local lock before attempting to acquire the
     * corresponding global lock in CXL shared memory.
     *
     * @param lock_id ID of the lock to acquire (0 to lock_count-1).
     * @return LockResult::SUCCESS if the lock was acquired.
     *         ERROR_INVALID_PARAM if lock_id is out of range.
     */
    LockResult acquire(LockId lock_id);

    /**
     * @brief Release a local lock by lock ID.
     *
     * Releases the lock, allowing other threads on this node to acquire it.
     * This is a wrapper around pthread_mutex_unlock().
     *
     * This should be called after releasing the global lock (if held), as
     * the last step in the lock release sequence.
     *
     * @param lock_id ID of the lock to release (0 to lock_count-1).
     * @return LockResult::SUCCESS if the lock was released.
     *         ERROR_INVALID_PARAM if lock_id is out of range.
     *         ERROR_NOT_OWNER if the calling thread does not hold the lock.
     */
    LockResult release(LockId lock_id);

    /**
     * @brief Try to acquire a local lock (non-blocking).
     *
     * Attempts to acquire the lock without blocking. If the lock is already
     * held by another thread, returns immediately with an error.
     *
     * This is useful for timeout-based or speculative lock acquisition
     * strategies where the caller wants to avoid blocking indefinitely.
     *
     * @param lock_id ID of the lock to try to acquire.
     * @return LockResult::SUCCESS if the lock was acquired immediately.
     *         ERROR_INVALID_PARAM if lock_id is out of range.
     *         ERROR_BUSY if the lock is currently held by another thread.
     */
    LockResult try_acquire(LockId lock_id);

    /**
     * @brief Check if a lock is held by the calling thread.
     *
     * Queries whether the specified lock is currently owned by the calling
     * thread. This is useful for assertions and debugging, but should NOT
     * be used as a synchronization primitive (use try_acquire() instead).
     *
     * Note: The accuracy of this check depends on the pthread implementation.
     * Some implementations may not support owner tracking for plain mutexes.
     *
     * @param lock_id ID of the lock to check.
     * @return true if the calling thread holds the lock.
     *         false if the lock is not held by this thread, is held by
     *         another thread, is unlocked, or lock_id is out of range.
     */
    bool is_held_by_this_thread(LockId lock_id) const;

private:
    /**
     * @brief Opaque implementation pointer (PIMPL idiom).
     *
     * Hides the internal array storage (pthread_mutex_t*), the lock count,
     * and any platform-specific mutex attributes from the header.
     */
    class Impl;
    Impl* impl_;
};

} // namespace cxl_lock
