/**
 * @file distributed_lock.cpp
 * @brief Implementation of the main distributed lock system interface.
 */

#include "distributed_lock.h"
#include "cxl_memory_pool.h"
#include "two_tier_lock.h"
#include "cas_global_lock.h"
#include "faa_global_lock.h"
#include "mcs_global_lock.h"
#include "qspinlock_global_lock.h"
#include <cstring>
#include <cstdio>

namespace cxl_lock {

// =========================================================================
// PImpl Implementation Class
// =========================================================================

class DistributedLockSystem::Impl {
public:
    Impl() = default;
    ~Impl() {
        shutdown();
    }

    LockResult initialize(const Config& config, bool as_lock_manager);
    void shutdown();

    LockResult acquire_lock(LockId lock_id, uint64_t timeout_ms);
    LockResult release_lock(LockId lock_id);
    LockResult try_lock(LockId lock_id);

    LockResult acquire_data_lock(const void* data_key, size_t key_len,
                                 uint64_t timeout_ms);
    LockResult release_data_lock(const void* data_key, size_t key_len);
    LockId get_data_lock_id(const void* data_key, size_t key_len);

    bool is_lock_manager() const { return is_lock_manager_; }
    NodeId get_node_id() const { return node_id_; }
    const char* get_algorithm_name() const;
    LockManager::Stats get_lock_manager_stats() const;

    LockResult initiate_fault_recovery();
    LockResult check_node_health(NodeId node_id);

    bool is_initialized() const { return initialized_; }

private:
    std::unique_ptr<CxlMemoryPool>          pool_;
    std::unique_ptr<LocalLockArray>         local_locks_;
    std::unique_ptr<TwoTierLock>            two_tier_lock_;
    std::unique_ptr<LockManager>            lock_manager_;
    std::shared_ptr<GlobalLockAlgorithm>    algorithm_;

    Config                                  config_;
    NodeId                                  node_id_ = INVALID_NODE_ID;
    bool                                    is_lock_manager_ = false;
    bool                                    initialized_ = false;
    bool                                    lock_manager_started_ = false;
};

// =========================================================================
// Impl: Initialization
// =========================================================================

LockResult DistributedLockSystem::Impl::initialize(const Config& config,
                                                    bool as_lock_manager) {
    if (initialized_) {
        return LockResult::ERROR_UNKNOWN;
    }

    config_ = config;
    node_id_ = static_cast<NodeId>(config.node_id);
    is_lock_manager_ = as_lock_manager;

    // --- Step 1: Open CXL shared memory pool ---
    pool_ = std::make_unique<CxlMemoryPool>();

    LockResult rc;
    if (config.use_file_backed_shm) {
        rc = pool_->open_file_backed(config.shm_backing_file,
                                     config.shm_size,
                                     config.consistency_mode);
    } else {
        rc = pool_->open(config.shm_device_path,
                         config.shm_size,
                         config.consistency_mode);
        if (rc != LockResult::SUCCESS) {
            // Device open failed; fall back to file-backed for testing.
            rc = pool_->open_file_backed(config.shm_backing_file,
                                          config.shm_size,
                                          config.consistency_mode);
        }
    }

    if (rc != LockResult::SUCCESS) {
        return rc;
    }

    // --- Step 2: Create the global lock algorithm ---
    if (config.lock_algorithm == "cas" || config.lock_algorithm == "CAS") {
        algorithm_ = std::make_shared<CasGlobalLockAlgorithm>();
    } else if (config.lock_algorithm == "faa" || config.lock_algorithm == "FAA") {
        algorithm_ = std::make_shared<FaaGlobalLockAlgorithm>();
    } else if (config.lock_algorithm == "mcs" || config.lock_algorithm == "MCS") {
        algorithm_ = std::make_shared<McsGlobalLockAlgorithm>();
    } else if (config.lock_algorithm == "qspinlock" || config.lock_algorithm == "QSPINLOCK") {
        algorithm_ = std::make_shared<QspinlockGlobalLockAlgorithm>();
    } else {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // --- Step 3: Initialize local lock array (per-node DRAM) ---
    local_locks_ = std::make_unique<LocalLockArray>();
    rc = local_locks_->initialize(config.max_locks);
    if (rc != LockResult::SUCCESS) {
        pool_->close();
        return rc;
    }

    // --- Step 4: Initialize two-tier lock ---
    two_tier_lock_ = std::make_unique<TwoTierLock>();
    rc = two_tier_lock_->initialize(
        node_id_,
        local_locks_.get(),
        pool_->get_shm(),
        algorithm_,
        0,  // state_offset: lock state starts at beginning of SHM
        config.max_locks,
        config.max_nodes);

    if (rc != LockResult::SUCCESS) {
        local_locks_->destroy();
        pool_->close();
        return rc;
    }

    // --- Step 5: Initialize lock manager (if this node is the manager) ---
    if (as_lock_manager) {
        lock_manager_ = std::make_unique<LockManager>();
        // Create a separate algorithm instance for the lock manager.
        // The lock manager uses the same algorithm type but its own
        // instance for thread safety.
        std::unique_ptr<GlobalLockAlgorithm> mgr_algo;
        if (config.lock_algorithm == "cas" || config.lock_algorithm == "CAS") {
            mgr_algo = std::make_unique<CasGlobalLockAlgorithm>();
        } else if (config.lock_algorithm == "faa" || config.lock_algorithm == "FAA") {
            mgr_algo = std::make_unique<FaaGlobalLockAlgorithm>();
        } else if (config.lock_algorithm == "mcs" || config.lock_algorithm == "MCS") {
            mgr_algo = std::make_unique<McsGlobalLockAlgorithm>();
        } else if (config.lock_algorithm == "qspinlock" || config.lock_algorithm == "QSPINLOCK") {
            mgr_algo = std::make_unique<QspinlockGlobalLockAlgorithm>();
        } else {
            return LockResult::ERROR_INVALID_PARAM;
        }

        rc = lock_manager_->initialize(
            pool_->get_shm(),
            std::move(mgr_algo),
            config.max_locks,
            config.max_nodes,
            config.lock_manager_scan_interval_us);

        if (rc != LockResult::SUCCESS) {
            two_tier_lock_->shutdown();
            local_locks_->destroy();
            pool_->close();
            return rc;
        }

        // Start the lock manager's background scan thread.
        rc = lock_manager_->start();
        if (rc != LockResult::SUCCESS) {
            two_tier_lock_->shutdown();
            local_locks_->destroy();
            pool_->close();
            return rc;
        }
        lock_manager_started_ = true;
    }

    initialized_ = true;
    return LockResult::SUCCESS;
}

// =========================================================================
// Impl: Shutdown
// =========================================================================

void DistributedLockSystem::Impl::shutdown() {
    if (!initialized_) {
        return;
    }

    // Stop the lock manager first.
    if (lock_manager_started_ && lock_manager_) {
        lock_manager_->stop();
        lock_manager_started_ = false;
    }

    // Release resources in reverse order.
    if (two_tier_lock_) {
        two_tier_lock_->shutdown();
        two_tier_lock_.reset();
    }

    if (local_locks_) {
        local_locks_->destroy();
        local_locks_.reset();
    }

    algorithm_.reset();

    if (pool_) {
        pool_->close();
        pool_.reset();
    }

    lock_manager_.reset();
    initialized_ = false;
}

// =========================================================================
// Impl: Lock operations
// =========================================================================

LockResult DistributedLockSystem::Impl::acquire_lock(LockId lock_id,
                                                      uint64_t timeout_ms) {
    if (!initialized_) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }
    return two_tier_lock_->acquire(lock_id, timeout_ms);
}

LockResult DistributedLockSystem::Impl::release_lock(LockId lock_id) {
    if (!initialized_) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }
    return two_tier_lock_->release(lock_id);
}

LockResult DistributedLockSystem::Impl::try_lock(LockId lock_id) {
    if (!initialized_) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }
    return two_tier_lock_->try_acquire(lock_id);
}

// =========================================================================
// Impl: Data-key-based lock operations
// =========================================================================

LockResult DistributedLockSystem::Impl::acquire_data_lock(const void* data_key,
                                                          size_t key_len,
                                                          uint64_t timeout_ms) {
    LockId lid = get_data_lock_id(data_key, key_len);
    return acquire_lock(lid, timeout_ms);
}

LockResult DistributedLockSystem::Impl::release_data_lock(const void* data_key,
                                                          size_t key_len) {
    LockId lid = get_data_lock_id(data_key, key_len);
    return release_lock(lid);
}

LockId DistributedLockSystem::Impl::get_data_lock_id(const void* data_key,
                                                      size_t key_len) {
    return TwoTierLock::hash_data_to_lock_id(data_key, key_len,
                                              config_.max_locks);
}

// =========================================================================
// Impl: Queries
// =========================================================================

const char* DistributedLockSystem::Impl::get_algorithm_name() const {
    if (algorithm_) {
        return algorithm_->get_name();
    }
    return "<none>";
}

LockManager::Stats DistributedLockSystem::Impl::get_lock_manager_stats() const {
    if (lock_manager_) {
        return lock_manager_->get_stats();
    }
    return LockManager::Stats{};
}

// =========================================================================
// Impl: Fault recovery (reserved)
// =========================================================================

LockResult DistributedLockSystem::Impl::initiate_fault_recovery() {
    // Reserved for future implementation.
    // A full implementation would:
    //   1. Detect failed nodes via heartbeat timeouts
    //   2. Forcibly release locks held by failed nodes
    //   3. Notify waiting nodes of lock availability changes
    //   4. Elect a new lock manager if the current one failed
    return LockResult::SUCCESS;
}

LockResult DistributedLockSystem::Impl::check_node_health(NodeId node_id) {
    // Reserved for future implementation.
    // Would check a heartbeat timestamp in shared memory for the node.
    (void)node_id;
    return LockResult::SUCCESS;
}

// =========================================================================
// DistributedLockSystem: Public Interface (forwarding to Impl)
// =========================================================================

DistributedLockSystem::DistributedLockSystem()
    : impl_(new Impl()) {
}

DistributedLockSystem::~DistributedLockSystem() {
    delete impl_;
}

LockResult DistributedLockSystem::initialize(const Config& config,
                                              bool as_lock_manager) {
    return impl_->initialize(config, as_lock_manager);
}

void DistributedLockSystem::shutdown() {
    impl_->shutdown();
}

LockResult DistributedLockSystem::acquire_lock(LockId lock_id,
                                                uint64_t timeout_ms) {
    return impl_->acquire_lock(lock_id, timeout_ms);
}

LockResult DistributedLockSystem::release_lock(LockId lock_id) {
    return impl_->release_lock(lock_id);
}

LockResult DistributedLockSystem::try_lock(LockId lock_id) {
    return impl_->try_lock(lock_id);
}

LockResult DistributedLockSystem::acquire_data_lock(const void* data_key,
                                                     size_t key_len,
                                                     uint64_t timeout_ms) {
    return impl_->acquire_data_lock(data_key, key_len, timeout_ms);
}

LockResult DistributedLockSystem::release_data_lock(const void* data_key,
                                                     size_t key_len) {
    return impl_->release_data_lock(data_key, key_len);
}

LockId DistributedLockSystem::get_data_lock_id(const void* data_key,
                                                size_t key_len) {
    return impl_->get_data_lock_id(data_key, key_len);
}

bool DistributedLockSystem::is_lock_manager() const {
    return impl_->is_lock_manager();
}

NodeId DistributedLockSystem::get_node_id() const {
    return impl_->get_node_id();
}

const char* DistributedLockSystem::get_algorithm_name() const {
    return impl_->get_algorithm_name();
}

LockManager::Stats DistributedLockSystem::get_lock_manager_stats() const {
    return impl_->get_lock_manager_stats();
}

LockResult DistributedLockSystem::initiate_fault_recovery() {
    return impl_->initiate_fault_recovery();
}

LockResult DistributedLockSystem::check_node_health(NodeId node_id) {
    return impl_->check_node_health(node_id);
}

bool DistributedLockSystem::is_initialized() const {
    return impl_->is_initialized();
}

} // namespace cxl_lock
