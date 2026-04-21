/**
 * @file shared_memory.cpp
 * @brief Implementation of SharedMemoryRegion for CXL shared memory access.
 *
 * This file implements the SharedMemoryRegion class using mmap for both
 * DAX devices (real CXL hardware) and file-backed shared memory (simulation).
 *
 * Key features:
 *   - DAX device mapping with MAP_SYNC for persistent memory semantics
 *   - File-backed shared memory with MAP_SHARED for development/testing
 *   - Directory-based DAX simulation mode (for /dev/dax9.0-style paths)
 *   - Cache flush/fence operations using x86 CLFLUSH + MFENCE
 *   - Atomic operations via std::atomic_ref for C++20 compatibility
 *   - Proper PIMPL move semantics for resource ownership transfer
 */

#include "shared_memory.h"
#include "cxl_memory_pool.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace cxl_lock {

// ============================================================================
// Internal Implementation (PIMPL)
// ============================================================================

/**
 * @brief Private implementation data for SharedMemoryRegion.
 *
 * Hides platform-specific details (file descriptors, mmap flags) from the
 * public interface using the PIMPL (Pointer to IMPLementation) idiom.
 */
class SharedMemoryRegion::Impl {
public:
    /** File descriptor for the mapped device or file. -1 if not open. */
    int fd = -1;

    /** Base address of the mapped memory region. MAP_FAILED if not mapped. */
    void* base = MAP_FAILED;

    /** Total size of the mapped region in bytes. */
    size_t size = 0;

    /** Cache consistency mode (software flush vs hardware coherence). */
    ConsistencyMode consistency = ConsistencyMode::SOFTWARE_CONSISTENCY;

    /** True if the region is properly mapped and usable. */
    bool valid = false;

    /** The actual path used for opening (may differ from config for dirs). */
    std::string path;

    /**
     * @brief Detect if the given path refers to a DAX device.
     *
     * DAX devices have paths like "/dev/dax0.0", "/dev/dax1.0", etc.
     *
     * @param p Path to check.
     * @return true if the path contains "/dev/dax" (DAX device).
     */
    static bool is_dax_device(const std::string& p) {
        return p.find("/dev/dax") != std::string::npos;
    }

    /**
     * @brief Detect if the given path is a directory.
     *
     * Used to handle simulation mode where a DAX path (e.g., /dev/dax9.0)
     * is actually a directory. In this case, we create a file inside it.
     *
     * @param p Path to check.
     * @return true if the path exists and is a directory.
     */
    static bool is_directory(const std::string& p) {
        struct stat st;
        if (stat(p.c_str(), &st) != 0) {
            return false;
        }
        return S_ISDIR(st.st_mode);
    }
};

// ============================================================================
// Construction / Destruction / Move
// ============================================================================

/**
 * @brief Construct an unmapped SharedMemoryRegion.
 *
 * Allocates the private implementation object. The region is not usable
 * until init() is called successfully.
 */
SharedMemoryRegion::SharedMemoryRegion() : impl_(new Impl()) {}

/**
 * @brief Destructor - automatically unmaps and closes the region.
 *
 * Delegates to cleanup() to release all resources (munmap, close fd).
 */
SharedMemoryRegion::~SharedMemoryRegion() {
    cleanup();
    delete impl_;
}

/**
 * @brief Move constructor - transfers ownership of the mapped region.
 *
 * The source object is left in a default-constructed (unmapped) state.
 *
 * @param other The SharedMemoryRegion to move from.
 */
SharedMemoryRegion::SharedMemoryRegion(SharedMemoryRegion&& other) noexcept
    : impl_(other.impl_) {
    other.impl_ = new Impl();  // Leave other in valid empty state
}

/**
 * @brief Move assignment - transfers ownership of the mapped region.
 *
 * Cleans up any existing mapping before taking ownership of the other's
 * resources. The source object is left in a default-constructed state.
 *
 * @param other The SharedMemoryRegion to move from.
 * @return Reference to this object.
 */
SharedMemoryRegion& SharedMemoryRegion::operator=(SharedMemoryRegion&& other) noexcept {
    if (this != &other) {
        cleanup();
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = new Impl();  // Leave other in valid empty state
    }
    return *this;
}

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize the shared memory region by mapping the backing store.
 *
 * Opens the device/file specified in config and maps it into the process
 * address space using mmap(). Supports three modes:
 *
 *   1. DAX device (real CXL): Opens with O_RDWR, maps with MAP_SYNC.
 *   2. File-backed (simulation): Opens with O_CREAT|O_RDWR, ftruncate.
 *   3. Directory DAX (special simulation): If /dev/dax path is a directory,
 *      creates shm.bin inside it and uses that as a regular file.
 *
 * @param config Configuration parameters for the mapping.
 * @return LockResult::SUCCESS if mapped successfully.
 *         ERROR_INVALID_PARAM if config parameters are invalid.
 *         ERROR_SHM_FAILURE if the file could not be opened or mapped.
 */
LockResult SharedMemoryRegion::init(const Config& config) {
    // Validate configuration parameters
    if (config.size == 0) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Clean up any existing mapping before re-initializing
    if (impl_->valid) {
        cleanup();
    }

    // Determine the effective path to open
    std::string effective_path = config.device_path;
    bool is_dax = Impl::is_dax_device(config.device_path);
    bool using_dir_simulation = false;

    // Handle special case: DAX path is a directory (simulation mode)
    // This occurs in virtualized environments where /dev/dax9.0 is a directory
    if (is_dax && Impl::is_directory(config.device_path)) {
        effective_path = config.device_path + "/shm.bin";
        is_dax = false;  // Use regular file semantics
        using_dir_simulation = true;
    }

    impl_->path = effective_path;
    impl_->size = config.size;
    impl_->consistency = config.consistency;

    // -------------------------------------------------------------------------
    // Step 1: Open the device or file
    // -------------------------------------------------------------------------
    int open_flags;
    if (is_dax && !using_dir_simulation) {
        // Real DAX device: open read-write
        open_flags = O_RDWR;
    } else {
        // Regular file or directory simulation: create if needed, read-write
        open_flags = O_CREAT | O_RDWR;
    }

    mode_t open_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

    impl_->fd = ::open(effective_path.c_str(), open_flags, open_mode);
    if (impl_->fd < 0) {
        // Failed to open the backing store
        cleanup();
        return LockResult::ERROR_SHM_FAILURE;
    }

    // -------------------------------------------------------------------------
    // Step 2: For regular files (not DAX), extend to the requested size
    // -------------------------------------------------------------------------
    if (!is_dax || using_dir_simulation) {
        if (ftruncate(impl_->fd, static_cast<off_t>(config.size)) != 0) {
            cleanup();
            return LockResult::ERROR_SHM_FAILURE;
        }
    }

    // -------------------------------------------------------------------------
    // Step 3: mmap the file/device into the address space
    // -------------------------------------------------------------------------
    int mmap_flags = MAP_SHARED;

    if (is_dax && !using_dir_simulation) {
        // For real DAX devices, try MAP_SYNC for synchronous mapping.
        // MAP_SYNC requires MAP_SHARED_VALIDATE (which includes MAP_SHARED).
        // If the kernel doesn't support it, fall back to regular MAP_SHARED.
#ifdef MAP_SHARED_VALIDATE
        void* sync_base = mmap(nullptr, config.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED_VALIDATE | MAP_SYNC,
                               impl_->fd, 0);
        if (sync_base != MAP_FAILED) {
            impl_->base = sync_base;
        } else {
            // Fall back to regular MAP_SHARED
            impl_->base = mmap(nullptr, config.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, impl_->fd, 0);
        }
#else
        // MAP_SHARED_VALIDATE not available, use regular MAP_SHARED
        impl_->base = mmap(nullptr, config.size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, impl_->fd, 0);
#endif
    } else {
        // File-backed: use regular MAP_SHARED (goes through page cache)
        impl_->base = mmap(nullptr, config.size, PROT_READ | PROT_WRITE,
                           mmap_flags, impl_->fd, 0);
    }

    if (impl_->base == MAP_FAILED) {
        cleanup();
        return LockResult::ERROR_SHM_FAILURE;
    }

    // -------------------------------------------------------------------------
    // Step 4: Mark the region as valid
    // -------------------------------------------------------------------------
    impl_->valid = true;
    return LockResult::SUCCESS;
}

// ============================================================================
// Cleanup
// ============================================================================

/**
 * @brief Clean up the shared memory region.
 *
 * Unmaps the memory region (if mapped) and closes the file descriptor
 * (if open). Resets all internal state to default values. Safe to call
 * multiple times or on an unmapped region.
 */
void SharedMemoryRegion::cleanup() {
    // Unmap the memory region if it was successfully mapped
    if (impl_->base != MAP_FAILED && impl_->base != nullptr) {
        munmap(impl_->base, impl_->size);
        impl_->base = MAP_FAILED;
    }

    // Close the file descriptor if it was successfully opened
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }

    // Reset all state to default values
    impl_->size = 0;
    impl_->valid = false;
    impl_->path.clear();
    impl_->consistency = ConsistencyMode::SOFTWARE_CONSISTENCY;
}

// ============================================================================
// Memory Access
// ============================================================================

/**
 * @brief Get the base address of the mapped region.
 *
 * @return Pointer to the beginning of the mapped memory, or nullptr
 *         if the region is not mapped.
 */
void* SharedMemoryRegion::get_base_address() const {
    return impl_->valid ? impl_->base : nullptr;
}

/**
 * @brief Get the total size of the mapped region in bytes.
 *
 * @return Size in bytes, or 0 if the region is not mapped.
 */
size_t SharedMemoryRegion::get_size() const {
    return impl_->valid ? impl_->size : 0;
}

/**
 * @brief Convert a byte offset to a pointer within the mapped region.
 *
 * Validates that the offset is within the bounds of the mapped region
 * before computing the pointer.
 *
 * @param offset Byte offset from the base of the mapped region.
 * @return Pointer to the corresponding memory location, or nullptr
 *         if the offset is out of bounds or the region is not mapped.
 */
void* SharedMemoryRegion::offset_to_ptr(ShmOffset offset) const {
    if (!impl_->valid || impl_->base == MAP_FAILED || impl_->base == nullptr) {
        return nullptr;
    }
    if (offset >= impl_->size) {
        return nullptr;
    }
    return static_cast<char*>(impl_->base) + offset;
}

/**
 * @brief Convert a pointer to a byte offset within the mapped region.
 *
 * Validates that the pointer falls within the mapped region before
 * computing the offset. This is the inverse of offset_to_ptr().
 *
 * @param ptr Pointer to a location within the mapped region.
 * @return Byte offset from the base, or INVALID_OFFSET if the pointer
 *         is not within the mapped region or is null.
 */
ShmOffset SharedMemoryRegion::ptr_to_offset(void* ptr) const {
    if (!impl_->valid || ptr == nullptr || impl_->base == MAP_FAILED) {
        return INVALID_OFFSET;
    }

    char* cptr = static_cast<char*>(ptr);
    char* base = static_cast<char*>(impl_->base);

    // Validate pointer is within the mapped region
    if (cptr < base || cptr >= base + impl_->size) {
        return INVALID_OFFSET;
    }

    return static_cast<ShmOffset>(cptr - base);
}

// ============================================================================
// Cache Consistency Operations
// ============================================================================

/**
 * @brief Flush cache lines to ensure writes are visible to other nodes.
 *
 * For HARDWARE_CONSISTENCY mode: no-op (CXL fabric handles coherence).
 *
 * For SOFTWARE_CONSISTENCY mode: issues CLFLUSH instructions for every
 * cache line in the specified range, followed by MFENCE. This ensures
 * that data reaches the CXL memory fabric before the function returns.
 *
 * @param addr Start address of the range to flush.
 * @param size Number of bytes to flush.
 */
void SharedMemoryRegion::flush_cache(void* addr, size_t size) {
    if (!impl_->valid || addr == nullptr || size == 0) {
        return;
    }

    // Hardware consistency mode: no flush needed - CXL fabric handles it
    if (impl_->consistency == ConsistencyMode::HARDWARE_CONSISTENCY) {
        return;
    }

    // Software consistency mode: explicit cache flush using CLFLUSH
    flush_cache_region(addr, size);
}

/**
 * @brief Insert a full memory fence for ordering guarantees.
 *
 * Issues an x86 MFENCE instruction to ensure all preceding memory
 * operations are globally visible before any subsequent operations.
 * This is essential for correct ordering of shared memory accesses
 * across the CXL fabric.
 */
void SharedMemoryRegion::memory_fence() {
    __asm__ volatile("mfence" ::: "memory");
}

/**
 * @brief Invalidate cache lines to read fresh data from shared memory.
 *
 * For HARDWARE_CONSISTENCY mode: no-op.
 *
 * For SOFTWARE_CONSISTENCY mode: issues CLFLUSH to invalidate the
 * specified cache lines, forcing a re-fetch from CXL memory on the
 * next access. This is used before reading variables that may have
 * been written by other nodes.
 *
 * @param addr Start address of the range to invalidate.
 * @param size Number of bytes to invalidate.
 */
void SharedMemoryRegion::invalidate_cache(void* addr, size_t size) {
    // CLFLUSH both flushes and invalidates the cache line, so we reuse
    // the same implementation as flush_cache(). After CLFLUSH, the next
    // read to this address will fetch fresh data from memory.
    flush_cache(addr, size);
}

// ============================================================================
// Atomic Operations (templates with explicit instantiations)
// ============================================================================

/**
 * @brief Perform an atomic load from shared memory.
 *
 * Uses std::atomic_ref (C++20) to perform atomic loads on memory that
 * may not have been declared as std::atomic. This is essential for
 * shared memory regions where the underlying storage is raw bytes.
 *
 * @tparam T    The value type to load (must be trivially copyable).
 * @param addr  Address to load from (must be properly aligned for T).
 * @param order Memory ordering for the load.
 * @return The loaded value.
 */
template<typename T>
T SharedMemoryRegion::atomic_load(void* addr, std::memory_order order) {
    std::atomic_ref<T> ref(*static_cast<T*>(addr));
    return ref.load(order);
}

/**
 * @brief Perform an atomic store to shared memory.
 *
 * Uses std::atomic_ref (C++20) for atomic stores. The caller should
 * follow this with flush_cache() for visibility to other nodes in
 * software consistency mode.
 *
 * @tparam T    The value type to store (must be trivially copyable).
 * @param addr  Address to store to (must be properly aligned for T).
 * @param value The value to store.
 * @param order Memory ordering for the store.
 */
template<typename T>
void SharedMemoryRegion::atomic_store(void* addr, T value, std::memory_order order) {
    std::atomic_ref<T> ref(*static_cast<T*>(addr));
    ref.store(value, order);
}

/**
 * @brief Perform an atomic compare-and-exchange on shared memory.
 *
 * Atomically compares the value at addr with expected, and if equal,
 * replaces it with desired. Uses seq_cst memory ordering for both
 * success and failure cases for maximum safety in distributed locking.
 *
 * @tparam T       The value type (must be trivially copyable).
 * @param addr     Address of the value to compare-and-swap.
 * @param expected The expected current value.
 * @param desired  The value to write if comparison succeeds.
 * @return true if the swap succeeded, false otherwise.
 */
template<typename T>
bool SharedMemoryRegion::compare_exchange(void* addr, T expected, T desired) {
    std::atomic_ref<T> ref(*static_cast<T*>(addr));
    T exp = expected;
    return ref.compare_exchange_strong(exp, desired,
                                       std::memory_order_seq_cst,
                                       std::memory_order_seq_cst);
}

/**
 * @brief Perform an atomic fetch-and-add on shared memory.
 *
 * Atomically adds value to the variable at addr and returns the
 * previous value. Uses seq_cst memory ordering.
 *
 * @tparam T    The value type (must be an integral type).
 * @param addr  Address of the variable to modify.
 * @param value The value to add.
 * @return The value of the variable before the addition.
 */
template<typename T>
T SharedMemoryRegion::fetch_add(void* addr, T value) {
    std::atomic_ref<T> ref(*static_cast<T*>(addr));
    return ref.fetch_add(value, std::memory_order_seq_cst);
}

// ============================================================================
// Explicit Template Instantiations
// ============================================================================

// uint32_t instantiations
/** @cond */
template uint32_t SharedMemoryRegion::atomic_load<uint32_t>(void*, std::memory_order);
template void SharedMemoryRegion::atomic_store<uint32_t>(void*, uint32_t, std::memory_order);
template bool SharedMemoryRegion::compare_exchange<uint32_t>(void*, uint32_t, uint32_t);
template uint32_t SharedMemoryRegion::fetch_add<uint32_t>(void*, uint32_t);
/** @endcond */

// uint64_t instantiations
/** @cond */
template uint64_t SharedMemoryRegion::atomic_load<uint64_t>(void*, std::memory_order);
template void SharedMemoryRegion::atomic_store<uint64_t>(void*, uint64_t, std::memory_order);
template bool SharedMemoryRegion::compare_exchange<uint64_t>(void*, uint64_t, uint64_t);
template uint64_t SharedMemoryRegion::fetch_add<uint64_t>(void*, uint64_t);
/** @endcond */

// ============================================================================
// Query
// ============================================================================

/**
 * @brief Check if the region is properly mapped and usable.
 *
 * @return true if init() was called successfully and cleanup() has not
 *         been called since.
 */
bool SharedMemoryRegion::is_valid() const {
    return impl_->valid;
}

/**
 * @brief Get the cache consistency mode of this region.
 *
 * @return The consistency mode specified during init(), or
 *         SOFTWARE_CONSISTENCY if the region is not initialized.
 */
ConsistencyMode SharedMemoryRegion::get_consistency_mode() const {
    return impl_->consistency;
}

} // namespace cxl_lock
