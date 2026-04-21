/**
 * @file lock_manager.h
 * @brief Lock Manager for the CXL distributed lock system.
 *
 * The Lock Manager is a singleton component that runs on ONE designated node
 * (typically node 0). Its responsibility is to scan global lock entries in
 * CXL shared memory and grant locks to waiting nodes.
 *
 * Usage:
 *   1. Create a LockManager instance
 *   2. Call initialize() with the shared memory region and algorithm
 *   3. Call start() to begin the background scan thread
 *   4. Call stop() before shutdown
 */

#pragma once

#include "lock_interface.h"
#include <atomic>
#include <cstdint>
#include <thread>
#include <memory>
#include <mutex>

namespace cxl_lock {

// (no forward declarations needed)

/**
 * @brief Singleton lock manager that grants global locks to waiting nodes.
 *
 * The lock manager periodically scans all lock entries in shared memory.
 * For each lock, it checks if any node is waiting and the lock is free.
 * If so, it grants the lock to the appropriate waiting node.
 */
class LockManager {
public:
    /// Statistics counters for monitoring and debugging.
    struct Stats {
        uint64_t grants_total = 0;                          ///< Total grants
        uint64_t grants_by_lock[MAX_LOCK_COUNT] = {0};     ///< Per-lock grants
        uint64_t scan_cycles = 0;                           ///< Total scan iterations
        uint64_t idle_cycles = 0;                           ///< Scans with no work

        Stats() = default;
    };

    LockManager();
    ~LockManager();

    // Non-copyable, non-movable (singleton semantics)
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;
    LockManager(LockManager&&) = delete;
    LockManager& operator=(LockManager&&) = delete;

    /**
     * @brief Initialize the lock manager.
     *
     * @param shm              Shared memory region containing lock state
     * @param pool             CXL memory pool (for metadata)
     * @param algorithm        Global lock algorithm implementation
     * @param max_locks        Maximum number of locks to manage
     * @param node_count       Number of nodes in the system
     * @param scan_interval_us  Sleep between scans (microseconds)
     * @return LockResult::SUCCESS on success
     */
    LockResult initialize(SharedMemoryRegion* shm,
                          std::unique_ptr<GlobalLockAlgorithm> algorithm,
                          uint32_t max_locks,
                          uint32_t node_count,
                          uint32_t scan_interval_us = 100);

    /**
     * @brief Start the lock manager background thread.
     * @return LockResult::SUCCESS on success
     */
    LockResult start();

    /**
     * @brief Stop the lock manager background thread.
     * @return LockResult::SUCCESS on success
     */
    LockResult stop();

    /**
     * @brief Run one scan cycle. Can be called directly instead of start().
     *
     * This is useful for testing or for integrating the scan into an
     * existing event loop.
     */
    void run_single_scan();

    /**
     * @brief Main loop - blocks until stop() is called.
     *
     * Normally invoked internally by start() in a background thread.
     */
    void run();

    /// Check if the lock manager is currently running.
    bool is_running() const;

    /// Get a copy of the current statistics.
    Stats get_stats() const;

    /// Reset all statistics to zero.
    void reset_stats();

    /// Get the name of the algorithm in use.
    const char* get_algorithm_name() const;

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Scan a single lock and grant if possible.
    void scan_and_grant_lock(LockId lock_id);

    /// Check if we are the registered lock manager (singleton check).
    bool acquire_manager_role();
    void release_manager_role();

    // ------------------------------------------------------------------
    // Member data
    // ------------------------------------------------------------------

    SharedMemoryRegion*                shm_ = nullptr;
    std::unique_ptr<GlobalLockAlgorithm> algorithm_;
    uint32_t                           max_locks_ = 0;
    uint32_t                           node_count_ = 0;
    uint32_t                           scan_interval_us_ = 100;
    ShmOffset                          state_offset_ = 0;

    std::atomic<bool>                  running_{false};
    std::atomic<bool>                  stop_requested_{false};
    std::thread                        worker_thread_;

    mutable std::mutex                 stats_mutex_;
    Stats                              stats_;

    bool                               initialized_ = false;
    bool                               is_manager_ = false;
};

} // namespace cxl_lock
