/**
 * @file cxl_memory_pool.h
 * @brief CXL memory pool and cache flush utilities.
 *
 * Provides standalone CxlMemoryPool that manages a SharedMemoryRegion
 * internally, plus inline cache flush utilities for x86.
 */

#pragma once

#include "lock_types.h"
#include "shared_memory.h"
#include <string>
#include <memory>

namespace cxl_lock {

// ---------------------------------------------------------------------------
// Inline cache flush utilities (x86 CLFLUSH + MFENCE)
// ---------------------------------------------------------------------------

/// Flush a single cache line using CLFLUSH.
inline void flush_cache_line(void* addr) {
    __asm__ volatile("clflush %0" : : "m"(*(reinterpret_cast<char*>(addr))));
}

/// Flush all cache lines in [addr, addr+size) and issue MFENCE.
inline void flush_cache_region(void* addr, size_t size) {
    char* ptr = reinterpret_cast<char*>(addr);
    char* end = ptr + size;
    for (; ptr < end; ptr += 64) {
        __asm__ volatile("clflush %0" : : "m"(*ptr));
    }
    __asm__ volatile("mfence" ::: "memory");
}

// ---------------------------------------------------------------------------
// CxlMemoryPool
// ---------------------------------------------------------------------------

/**
 * @brief Manages the CXL shared memory pool lifecycle.
 *
 * Wraps SharedMemoryRegion with convenient open/close methods for
 * both DAX device and file-backed shared memory.
 */
class CxlMemoryPool {
public:
    CxlMemoryPool();
    ~CxlMemoryPool();

    // Non-copyable
    CxlMemoryPool(const CxlMemoryPool&) = delete;
    CxlMemoryPool& operator=(const CxlMemoryPool&) = delete;

    // Movable
    CxlMemoryPool(CxlMemoryPool&& other) noexcept;
    CxlMemoryPool& operator=(CxlMemoryPool&& other) noexcept;

    /// Open a DAX device (real CXL hardware).
    LockResult open(const std::string& device_path,
                    size_t size,
                    ConsistencyMode consistency = ConsistencyMode::SOFTWARE_CONSISTENCY);

    /// Open a file-backed mapping (for testing).
    LockResult open_file_backed(const std::string& file_path,
                                size_t size,
                                ConsistencyMode consistency = ConsistencyMode::SOFTWARE_CONSISTENCY);

    /// Close and unmap.
    void close();

    /// Check if the pool is open.
    bool is_open() const;

    /// Get the underlying SharedMemoryRegion (for algorithm use).
    SharedMemoryRegion* get_shm();

    /// Get the underlying SharedMemoryRegion (const).
    const SharedMemoryRegion* get_shm() const;

    /// Get the mapped size.
    size_t get_size() const;

    /// Get the consistency mode.
    ConsistencyMode get_consistency_mode() const;

private:
    std::unique_ptr<SharedMemoryRegion> shm_;
    ConsistencyMode                     consistency_mode_;
    size_t                              size_;
    bool                                is_open_;
};

} // namespace cxl_lock
