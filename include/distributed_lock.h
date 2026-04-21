/**
 * @file distributed_lock.h
 * @brief Main unified interface for the CXL distributed lock system.
 *
 * This is the top-level API that applications use to interact with the
 * distributed lock system. It wraps the TwoTierLock, LockManager, and
 * CxlMemoryPool components into a single easy-to-use interface.
 *
 * Usage example:
 *   @code
 *   DistributedLockSystem dls;
 *   DistributedLockSystem::Config config;
 *   config.node_id = 0;
 *   config.max_nodes = 4;
 *   dls.initialize(config, true);  // Node 0 is lock manager
 *   dls.acquire_lock(42);
 *   // ... critical section ...
 *   dls.release_lock(42);
 *   dls.shutdown();
 *   @endcode
 */

#pragma once

#include "lock_interface.h"
#include "lock_manager.h"
#include <string>
#include <cstdint>

namespace cxl_lock {

// Forward declarations
class CxlMemoryPool;
class TwoTierLock;
class LocalLockArray;

/**
 * @brief Main distributed lock system interface.
 *
 * This class provides a unified, easy-to-use interface for the entire
 * CXL distributed lock system. It manages the lifecycle of the underlying
 * components (memory pool, local locks, global lock algorithm, two-tier
 * lock, and optionally the lock manager).
 */
class DistributedLockSystem {
public:
    // ------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------

    struct Config {
        /// Path to the CXL device (e.g., "/dev/dax0.0")
        std::string shm_device_path = "/dev/dax0.0";

        /// Size of the shared memory region to map
        size_t shm_size = 64 * 1024 * 1024;  // 64 MB default

        /// Maximum number of compute nodes in the system
        uint32_t max_nodes = 16;

        /// Maximum number of locks
        uint32_t max_locks = 256;

        /// Lock algorithm: "cas" or "faa"
        std::string lock_algorithm = "cas";

        /// This node's ID
        uint32_t node_id = 0;

        /// Consistency mode
        ConsistencyMode consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;

        /// Lock manager scan interval (microseconds)
        uint32_t lock_manager_scan_interval_us = 100;

        /// Use file-backed shared memory for testing (instead of CXL device)
        bool use_file_backed_shm = false;

        /// Backing file path (only used if use_file_backed_shm is true)
        std::string shm_backing_file = "/tmp/cxl_distributed_lock.shm";
    };

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    DistributedLockSystem();
    ~DistributedLockSystem();

    // Non-copyable
    DistributedLockSystem(const DistributedLockSystem&) = delete;
    DistributedLockSystem& operator=(const DistributedLockSystem&) = delete;

    /**
     * @brief Initialize this node's connection to the distributed lock system.
     *
     * @param config            System configuration
     * @param as_lock_manager   If true, this node runs the lock manager
     * @return LockResult::SUCCESS on success
     */
    LockResult initialize(const Config& config, bool as_lock_manager = false);

    /// Shutdown the system and release all resources.
    void shutdown();

    // ------------------------------------------------------------------
    // Lock operations by ID
    // ------------------------------------------------------------------

    /**
     * @brief Acquire a lock by its ID (blocking).
     * @param lock_id      Lock to acquire
     * @param timeout_ms   Timeout in milliseconds (0 = no timeout)
     * @return LockResult::SUCCESS on success
     */
    LockResult acquire_lock(LockId lock_id, uint64_t timeout_ms = 0);

    /**
     * @brief Release a lock by its ID.
     * @param lock_id  Lock to release
     * @return LockResult::SUCCESS on success
     */
    LockResult release_lock(LockId lock_id);

    /**
     * @brief Try to acquire a lock (non-blocking).
     * @param lock_id  Lock to try acquiring
     * @return LockResult::SUCCESS if acquired, ERROR_BUSY if not available
     */
    LockResult try_lock(LockId lock_id);

    // ------------------------------------------------------------------
    // Lock operations by data key (hash-based)
    // ------------------------------------------------------------------

    /**
     * @brief Acquire a lock associated with a data key (blocking).
     *
     * The data key is hashed to determine which lock ID to use.
     *
     * @param data_key   Pointer to the data key
     * @param key_len    Length of the data key in bytes
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return LockResult::SUCCESS on success
     */
    LockResult acquire_data_lock(const void* data_key, size_t key_len,
                                 uint64_t timeout_ms = 0);

    /**
     * @brief Release a lock associated with a data key.
     * @param data_key  Pointer to the data key
     * @param key_len   Length of the data key in bytes
     * @return LockResult::SUCCESS on success
     */
    LockResult release_data_lock(const void* data_key, size_t key_len);

    /**
     * @brief Get the lock ID associated with a data key (without locking).
     * @param data_key  Pointer to the data key
     * @param key_len   Length of the data key in bytes
     * @return The LockId that would be used for this data key
     */
    LockId get_data_lock_id(const void* data_key, size_t key_len);

    // ------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------

    /// Check if this node is the lock manager.
    bool is_lock_manager() const;

    /// Get this node's ID.
    NodeId get_node_id() const;

    /// Get the name of the lock algorithm in use.
    const char* get_algorithm_name() const;

    /// Get lock manager statistics (only valid on lock manager node).
    LockManager::Stats get_lock_manager_stats() const;

    // ------------------------------------------------------------------
    // Fault recovery (reserved for future implementation)
    // ------------------------------------------------------------------

    /**
     * @brief Initiate fault recovery procedure.
     *
     * This is reserved for future implementation. It would handle
     * scenarios where a node fails while holding locks.
     */
    LockResult initiate_fault_recovery();

    /**
     * @brief Check the health of a remote node.
     * @param node_id  Node to check
     * @return LockResult::SUCCESS if node is healthy
     */
    LockResult check_node_health(NodeId node_id);

    /// Check if the system is initialized.
    bool is_initialized() const;

private:
    class Impl;
    Impl* impl_;
};

} // namespace cxl_lock
