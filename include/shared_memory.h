/**
 * @file shared_memory.h
 * @brief Shared memory region abstraction for CXL memory pool access.
 *
 * This header provides the SharedMemoryRegion class, which abstracts the
 * details of mapping and accessing CXL shared memory. It supports both
 * direct DAX device mapping (for real CXL hardware) and file-based
 * simulation (for development and testing).
 *
 * The class handles:
 *   - Memory mapping via mmap (DAX devices or regular files)
 *   - Cache consistency management (software flush vs hardware coherence)
 *   - Atomic operations on shared memory locations
 *   - Offset-to-pointer translation for the shared memory layout
 *
 * Thread Safety: SharedMemoryRegion is thread-safe for concurrent reads
 * and writes to different addresses. Concurrent access to the same atomic
 * location is handled by the underlying atomic operations.
 *
 * @code
 *   // Example: mapping a CXL DAX device
 *   SharedMemoryRegion shm;
 *   SharedMemoryRegion::Config config;
 *   config.device_path = "/dev/dax0.0";
 *   config.size = 256 * 1024 * 1024;  // 256 MB
 *   config.consistency = ConsistencyMode::HARDWARE_CONSISTENCY;
 *   config.create = true;
 *   LockResult rc = shm.init(config);
 *   if (rc != LockResult::SUCCESS) { ... }
 *
 *   // Access shared memory
 *   void* header = shm.offset_to_ptr(0);
 *   uint32_t* flag = static_cast<uint32_t*>(shm.offset_to_ptr(64));
 *   uint32_t old_val = shm.fetch_add<uint32_t>(flag, 1);
 * @endcode
 */
#pragma once

#include "lock_types.h"
#include <string>
#include <atomic>

namespace cxl_lock {

// ---------------------------------------------------------------------------
// SharedMemoryRegion
// ---------------------------------------------------------------------------

/**
 * @brief Abstraction for a mapped region of CXL shared memory.
 *
 * Provides a unified interface for accessing shared memory regardless of
 * whether it is backed by a DAX device, a regular file, or anonymous
 * shared memory. All shared memory operations in the lock system go
 * through this class.
 *
 * The PIMPL idiom is used to hide implementation details (file descriptors,
 * mapping parameters, etc.) from the header.
 */
class SharedMemoryRegion {
public:
    /**
     * @brief Configuration parameters for initializing the shared memory region.
     *
     * This struct collects all parameters needed to map a shared memory region.
     * It is passed to init() to configure the mapping.
     */
    struct Config {
        /**
         * @brief Path to the backing store.
         *
         * For DAX devices: "/dev/daxX.Y" (e.g., "/dev/dax0.0")
         * For file-based simulation: path to a regular file
         * For anonymous shared memory: empty string (uses tmpfs/hugetlb)
         */
        std::string device_path;

        /**
         * @brief Total size of the shared memory region in bytes.
         *
         * Must be a multiple of the page size (typically 4096 bytes).
         * For DAX devices, this must not exceed the device capacity.
         */
        size_t size;

        /**
         * @brief Cache consistency mode for shared memory writes.
         *
         * SOFTWARE_CONSISTENCY: Explicit cache flush on every write.
         * HARDWARE_CONSISTENCY: Rely on CXL hardware coherence (no flush).
         */
        ConsistencyMode consistency;

        /**
         * @brief Whether to create a new mapping or attach to an existing one.
         *
         * If true: Creates/opens the backing file and maps it. For DAX devices,
         *          this initializes the device. The header should then be written.
         * If false: Attaches to an existing mapping. Assumes the backing file
         *           already exists and contains a valid layout.
         */
        bool create;
    };

    /**
     * @brief Construct an unmapped SharedMemoryRegion.
     *
     * The region is not usable until init() is called successfully.
     */
    SharedMemoryRegion();

    /**
     * @brief Destructor - automatically unmaps and closes the region.
     *
     * If the region was successfully mapped, cleanup() is called automatically.
     */
    ~SharedMemoryRegion();

    // Disable copy; shared memory regions are unique resources.
    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;

    // Enable move semantics for transfer of ownership.
    SharedMemoryRegion(SharedMemoryRegion&& other) noexcept;
    SharedMemoryRegion& operator=(SharedMemoryRegion&& other) noexcept;

    /**
     * @brief Initialize the shared memory region by mapping the backing store.
     *
     * Opens the device/file specified in config and maps it into the process
     * address space. For DAX devices, the mapping is direct (bypasses page cache).
     * For regular files, the mapping goes through the page cache.
     *
     * @param config Configuration parameters for the mapping.
     * @return LockResult::SUCCESS if the region was mapped successfully.
     *         ERROR_INVALID_PARAM if config parameters are invalid (e.g., size=0).
     *         ERROR_SHM_FAILURE if the file could not be opened or mapped.
     */
    LockResult init(const Config& config);

    /**
     * @brief Clean up the shared memory region.
     *
     * Unmaps the memory region and closes the backing file descriptor.
     * After cleanup, the region is no longer usable until init() is called again.
     * This is called automatically by the destructor.
     */
    void cleanup();

    /**
     * @brief Get the base address of the mapped region.
     *
     * @return Pointer to the beginning of the mapped memory, or nullptr
     *         if the region is not mapped.
     */
    void* get_base_address() const;

    /**
     * @brief Get the total size of the mapped region in bytes.
     *
     * @return Size in bytes, or 0 if the region is not mapped.
     */
    size_t get_size() const;

    /**
     * @brief Convert a byte offset to a pointer within the mapped region.
     *
     * The offset is relative to the base address of the region.
     * The returned pointer is valid for the lifetime of the mapping.
     *
     * @param offset Byte offset from the base of the mapped region.
     * @return Pointer to the corresponding memory location, or nullptr
     *         if the offset is out of bounds or the region is not mapped.
     */
    void* offset_to_ptr(ShmOffset offset) const;

    /**
     * @brief Convert a pointer to a byte offset within the mapped region.
     *
     * This is the inverse of offset_to_ptr(). It validates that the pointer
     * falls within the mapped region before computing the offset.
     *
     * @param ptr Pointer to a location within the mapped region.
     * @return Byte offset from the base, or INVALID_OFFSET if the pointer
     *         is not within the mapped region.
     */
    ShmOffset ptr_to_offset(void* ptr) const;

    /**
     * @brief Flush cache lines to ensure writes are visible to other nodes.
     *
     * For software consistency mode, this issues clflushopt or clwb instructions
     * for all cache lines covering the specified address range, followed by an
     * sfence. This ensures that the writes have reached the CXL memory fabric
     * before the function returns.
     *
     * For hardware consistency mode, this is a no-op (no flush needed).
     *
     * @param addr Start address of the range to flush (must be in mapped region).
     * @param size Number of bytes to flush.
     */
    void flush_cache(void* addr, size_t size);

    /**
     * @brief Insert a full memory fence for ordering guarantees.
     *
     * Issues an sfence (or equivalent) to ensure all preceding memory
     * operations are globally visible before any subsequent operations.
     * This is typically called after flush_cache() and before polling
     * on a shared variable.
     */
    void memory_fence();

    /**
     * @brief Invalidate cache lines to read fresh data from shared memory.
     *
     * For software consistency mode, this issues clflushopt to invalidate
     * the cache lines covering the specified range, forcing a re-fetch
     * from the CXL memory fabric on the next access. This is useful when
     * polling on a variable that was written by another node.
     *
     * For hardware consistency mode, this is a no-op.
     *
     * @param addr Start address of the range to invalidate.
     * @param size Number of bytes to invalidate.
     */
    void invalidate_cache(void* addr, size_t size);

    /**
     * @brief Perform an atomic load from shared memory.
     *
     * Loads a value of type T from the given address using the specified
     * memory ordering. The address must be properly aligned for type T.
     *
     * @tparam T     The value type to load (must be trivially copyable).
     * @param addr   Address to load from (must be in mapped region).
     * @param order  Memory ordering for the load operation.
     * @return The loaded value.
     */
    template<typename T>
    T atomic_load(void* addr, std::memory_order order = std::memory_order_seq_cst);

    /**
     * @brief Perform an atomic store to shared memory.
     *
     * Stores a value of type T to the given address using the specified
     * memory ordering. The address must be properly aligned for type T.
     * In software consistency mode, this also flushes the cache line.
     *
     * @tparam T     The value type to store (must be trivially copyable).
     * @param addr   Address to store to (must be in mapped region).
     * @param value  The value to store.
     * @param order  Memory ordering for the store operation.
     */
    template<typename T>
    void atomic_store(void* addr, T value, std::memory_order order = std::memory_order_seq_cst);

    /**
     * @brief Perform an atomic compare-and-exchange on shared memory.
     *
     * Atomically compares the value at addr with expected, and if equal,
     * replaces it with desired. Uses std::memory_order_seq_cst for both
     * success and failure cases.
     *
     * @tparam T      The value type (must be trivially copyable).
     * @param addr    Address of the value to compare-and-swap.
     * @param expected The expected current value.
     * @param desired  The value to write if the comparison succeeds.
     * @return true if the swap succeeded (value was equal to expected).
     *         false if the swap failed (value differed from expected).
     */
    template<typename T>
    bool compare_exchange(void* addr, T expected, T desired);

    /**
     * @brief Perform an atomic fetch-and-add on shared memory.
     *
     * Atomically adds the given value to the variable at addr and returns
     * the previous value. Uses std::memory_order_seq_cst.
     *
     * @tparam T     The value type (must be an integral type).
     * @param addr   Address of the variable to modify.
     * @param value  The value to add.
     * @return The value of the variable before the addition.
     */
    template<typename T>
    T fetch_add(void* addr, T value);

    /**
     * @brief Check if the region is properly mapped and usable.
     *
     * @return true if init() was called successfully and the region is mapped.
     *         false if the region has not been initialized or was cleaned up.
     */
    bool is_valid() const;

    /**
     * @brief Get the cache consistency mode of this region.
     *
     * @return The consistency mode specified during init(), or
     *         SOFTWARE_CONSISTENCY if the region is not initialized.
     */
    ConsistencyMode get_consistency_mode() const;

private:
    /**
     * @brief Opaque implementation pointer (PIMPL idiom).
     *
     * Hides platform-specific details (file descriptors, mapping flags,
     * DAX-specific handling) from the header.
     */
    class Impl;
    Impl* impl_;
};

} // namespace cxl_lock
