/**
 * @file cxl_memory_pool.cpp
 * @brief Implementation of CxlMemoryPool.
 */

#include "cxl_memory_pool.h"
#include <cstring>

namespace cxl_lock {

// =========================================================================
// Construction / Destruction
// =========================================================================

CxlMemoryPool::CxlMemoryPool()
    : shm_(std::make_unique<SharedMemoryRegion>()),
      consistency_mode_(ConsistencyMode::SOFTWARE_CONSISTENCY),
      size_(0),
      is_open_(false) {
}

CxlMemoryPool::~CxlMemoryPool() {
    close();
}

CxlMemoryPool::CxlMemoryPool(CxlMemoryPool&& other) noexcept
    : shm_(std::move(other.shm_)),
      consistency_mode_(other.consistency_mode_),
      size_(other.size_),
      is_open_(other.is_open_) {
    other.is_open_ = false;
    other.size_ = 0;
}

CxlMemoryPool& CxlMemoryPool::operator=(CxlMemoryPool&& other) noexcept {
    if (this != &other) {
        close();
        shm_ = std::move(other.shm_);
        consistency_mode_ = other.consistency_mode_;
        size_ = other.size_;
        is_open_ = other.is_open_;
        other.is_open_ = false;
        other.size_ = 0;
    }
    return *this;
}

// =========================================================================
// Open / Close
// =========================================================================

LockResult CxlMemoryPool::open(const std::string& device_path,
                               size_t size,
                               ConsistencyMode consistency) {
    if (is_open_) {
        return LockResult::ERROR_UNKNOWN;
    }
    if (size == 0) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    SharedMemoryRegion::Config config;
    config.device_path = device_path;
    config.size = size;
    config.consistency = consistency;
    config.create = true;

    LockResult rc = shm_->init(config);
    if (rc != LockResult::SUCCESS) {
        return rc;
    }

    size_ = size;
    consistency_mode_ = consistency;
    is_open_ = true;
    return LockResult::SUCCESS;
}

LockResult CxlMemoryPool::open_file_backed(const std::string& file_path,
                                           size_t size,
                                           ConsistencyMode consistency) {
    if (is_open_) {
        return LockResult::ERROR_UNKNOWN;
    }
    if (size == 0) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    SharedMemoryRegion::Config config;
    config.device_path = file_path;
    config.size = size;
    config.consistency = consistency;
    config.create = true;

    LockResult rc = shm_->init(config);
    if (rc != LockResult::SUCCESS) {
        return rc;
    }

    // Zero-initialize the shared memory for file-backed mode.
    void* base = shm_->get_base_address();
    if (base) {
        std::memset(base, 0, size);
        shm_->flush_cache(base, size);
        shm_->memory_fence();
    }

    size_ = size;
    consistency_mode_ = consistency;
    is_open_ = true;
    return LockResult::SUCCESS;
}

void CxlMemoryPool::close() {
    if (!is_open_) {
        return;
    }

    if (shm_) {
        if (consistency_mode_ == ConsistencyMode::SOFTWARE_CONSISTENCY) {
            void* base = shm_->get_base_address();
            if (base && size_ > 0) {
                shm_->flush_cache(base, size_);
            }
        }
        shm_->cleanup();
    }

    size_ = 0;
    is_open_ = false;
}

bool CxlMemoryPool::is_open() const {
    return is_open_;
}

SharedMemoryRegion* CxlMemoryPool::get_shm() {
    return shm_.get();
}

const SharedMemoryRegion* CxlMemoryPool::get_shm() const {
    return shm_.get();
}

size_t CxlMemoryPool::get_size() const {
    return size_;
}

ConsistencyMode CxlMemoryPool::get_consistency_mode() const {
    return consistency_mode_;
}

} // namespace cxl_lock
