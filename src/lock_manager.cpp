/**
 * @file lock_manager.cpp
 * @brief Implementation of the Lock Manager for CXL distributed locks.
 */

#include "lock_manager.h"
#include <cstring>
#include <thread>
#include <chrono>
#include <cstdio>

namespace cxl_lock {

// =========================================================================
// Construction / Destruction
// =========================================================================

LockManager::LockManager() = default;

LockManager::~LockManager() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

// =========================================================================
// Initialization
// =========================================================================

LockResult LockManager::initialize(SharedMemoryRegion* shm,
                                   std::unique_ptr<GlobalLockAlgorithm> algorithm,
                                   uint32_t max_locks,
                                   uint32_t node_count,
                                   uint32_t scan_interval_us) {
    if (shm == nullptr || algorithm == nullptr) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (max_locks == 0 || max_locks > MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_count == 0 || node_count > MAX_NODE_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (initialized_) {
        return LockResult::ERROR_UNKNOWN;
    }

    shm_              = shm;
    algorithm_        = std::move(algorithm);
    max_locks_        = max_locks;
    node_count_       = node_count;
    scan_interval_us_ = scan_interval_us;

    // Initialize the global lock state in shared memory.
    // state_offset of 0 means the state starts at the beginning of the
    // shared memory region dedicated to lock state.
    state_offset_ = 0;

    LockResult rc = algorithm_->initialize(shm_, state_offset_, node_count_);
    if (rc != LockResult::SUCCESS) {
        return rc;
    }

    initialized_ = true;

    // Try to become the designated lock manager (singleton).
    // In a real system, this would use a consensus protocol or
    // pre-configured node ID. Here we use a simple atomic flag.
    if (!acquire_manager_role()) {
        // Another node is already the lock manager; we can still
        // initialize but should not start scanning.
        return LockResult::SUCCESS;
    }

    is_manager_ = true;
    return LockResult::SUCCESS;
}

// =========================================================================
// Manager role (singleton enforcement)
// =========================================================================

bool LockManager::acquire_manager_role() {
    // In a real implementation, this would check/set a persistent flag
    // in CXL memory or use a consensus service. For this implementation,
    // we assume the caller ensures only one node calls start().
    return true;
}

void LockManager::release_manager_role() {
    // Clear the manager role flag in shared memory.
}

// =========================================================================
// Start / Stop
// =========================================================================

LockResult LockManager::start() {
    if (!initialized_) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }
    if (running_.load(std::memory_order_acquire)) {
        return LockResult::ERROR_UNKNOWN;
    }
    if (!is_manager_) {
        return LockResult::ERROR_NOT_OWNER;
    }

    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    worker_thread_ = std::thread(&LockManager::run, this);

    return LockResult::SUCCESS;
}

LockResult LockManager::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return LockResult::ERROR_LOCK_NOT_INITIALIZED;
    }

    stop_requested_.store(true, std::memory_order_release);

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    running_.store(false, std::memory_order_release);
    return LockResult::SUCCESS;
}

// =========================================================================
// Main scan loop
// =========================================================================

void LockManager::run() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        run_single_scan();

        // Sleep before next scan cycle.
        if (scan_interval_us_ > 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(scan_interval_us_));
        }
    }
}

void LockManager::run_single_scan() {
    bool any_work = false;

    for (LockId lid = 0; lid < max_locks_; ++lid) {
        NodeId grantee = algorithm_->check_grant(lid, shm_, state_offset_,
                                                  node_count_);
        if (grantee != INVALID_NODE_ID) {
            LockResult rc = algorithm_->grant_lock(grantee, lid, shm_,
                                                   state_offset_);
            if (rc == LockResult::SUCCESS) {
                any_work = true;

                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.grants_total++;
                stats_.grants_by_lock[lid]++;
            }
        }
    }

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.scan_cycles++;
    if (!any_work) {
        stats_.idle_cycles++;
    }
}

// =========================================================================
// Queries
// =========================================================================

bool LockManager::is_running() const {
    return running_.load(std::memory_order_acquire);
}

LockManager::Stats LockManager::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void LockManager::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats{};
}

const char* LockManager::get_algorithm_name() const {
    if (algorithm_) {
        return algorithm_->get_name();
    }
    return "<none>";
}

} // namespace cxl_lock
