/**
 * @file lock_interface.h
 * @brief Abstract lock interface and plugin architecture for the CXL Distributed Lock System.
 *
 * This is the KEY architectural file of the entire system. It defines the
 * plugin architecture that enables pluggable lock algorithms. Different
 * locking strategies (e.g., CAS-based MCS lock, FAA-based queue lock,
 * hierarchical locks) can be implemented by subclassing GlobalLockAlgorithm
 * and registering them via the LockAlgorithmRegistry.
 *
 * The two-tier locking mechanism works as follows:
 *   1. A thread first acquires its local DRAM-resident mutex (LocalLockArray).
 *   2. If the node does not already hold the global lock, it uses a
 *      GlobalLockAlgorithm to coordinate with other nodes via CXL shared memory.
 *   3. Once the global lock is granted, the thread proceeds with the critical section.
 *   4. On release, the global lock is released first, then the local mutex.
 *
 * New lock algorithms can be added without modifying any existing code by:
 *   1. Implementing the GlobalLockAlgorithm interface.
 *   2. Using the REGISTER_LOCK_ALGORITHM macro to auto-register the class.
 *
 * @code
 * // Example: implementing a custom lock algorithm
 * class MyCustomLock : public GlobalLockAlgorithm {
 * public:
 *     LockResult initialize(SharedMemoryRegion* shm, ShmOffset state_offset,
 *                           uint32_t node_count) override { ... }
 *     LockResult acquire_lock(NodeId node_id, LockId lock_id,
 *                             SharedMemoryRegion* shm, ShmOffset state_offset) override { ... }
 *     LockResult release_lock(NodeId node_id, LockId lock_id,
 *                             SharedMemoryRegion* shm, ShmOffset state_offset) override { ... }
 *     NodeId check_grant(LockId lock_id, SharedMemoryRegion* shm,
 *                        ShmOffset state_offset, uint32_t node_count) override { ... }
 *     LockResult grant_lock(NodeId node_id, LockId lock_id,
 *                           SharedMemoryRegion* shm, ShmOffset state_offset) override { ... }
 *     size_t get_state_size(uint32_t node_count) const override { ... }
 *     const char* get_name() const override { return "MyCustomLock"; }
 * };
 * REGISTER_LOCK_ALGORITHM("my_custom_lock", MyCustomLock);
 * @endcode
 */
#pragma once

#include "lock_types.h"
#include <memory>
#include <atomic>

namespace cxl_lock {

// ---------------------------------------------------------------------------
// Forward Declarations
// ---------------------------------------------------------------------------

/**
 * @brief Abstraction for the CXL shared memory region.
 *
 * Provides raw memory access, cache flush/fence operations, and atomic
 * operations on the shared memory pool. Defined in shared_memory.h.
 */
class SharedMemoryRegion;

// ---------------------------------------------------------------------------
// GlobalLockAlgorithm - The Core Plugin Interface
// ---------------------------------------------------------------------------

/**
 * @brief Abstract interface for global lock coordination algorithms.
 *
 * A GlobalLockAlgorithm implements the protocol for coordinating lock
 * acquisition and release across multiple nodes using CXL shared memory.
 * Each lock ID in the system can use a different algorithm, selected at
 * initialization time.
 *
 * The algorithm manages its own state within a region of shared memory
 * allocated by the CxlMemoryPool. The size of this state region is
 * reported by get_state_size() and varies by algorithm and node count.
 *
 * Thread Safety: Implementations must be safe for concurrent access from
 * multiple threads on the same node (each going through the same node ID)
 * and from multiple nodes simultaneously.
 *
 * Memory Ordering: All shared memory accesses must use appropriate memory
 * ordering (typically std::memory_order_seq_cst or acq_rel) to ensure
 * visibility across the CXL fabric.
 */
class GlobalLockAlgorithm {
public:
    /**
     * @brief Virtual destructor for proper cleanup of derived classes.
     */
    virtual ~GlobalLockAlgorithm() = default;

    /**
     * @brief Initialize the algorithm's state in shared memory.
     *
     * Called once per lock during system initialization (by the lock manager
     * on node 0). The algorithm should write its initial state structure to
     * the region starting at state_offset within the shared memory pool.
     *
     * @param shm             Pointer to the shared memory region.
     * @param state_offset    Byte offset within shared memory where the
     *                        algorithm's state should be stored.
     * @param node_count      Number of nodes that will participate in locking.
     *                        This determines the size of per-node arrays.
     * @return LockResult::SUCCESS on success, or an error code.
     */
    virtual LockResult initialize(
        SharedMemoryRegion* shm,
        ShmOffset           state_offset,
        uint32_t            node_count) = 0;

    /**
     * @brief Request acquisition of the global lock.
     *
     * Called by a node when it wants to acquire the global lock for a
     * specific lock ID. The algorithm should set the node's slot to the
     * WAITING state and perform any protocol-specific operations (e.g.,
     * enqueue in a queue, set a CAS flag).
     *
     * This is a non-blocking call that initiates the acquisition protocol.
     * The caller should subsequently poll or wait for the lock to be granted.
     *
     * @param node_id      ID of the node requesting the lock.
     * @param lock_id      ID of the lock to acquire.
     * @param shm          Pointer to the shared memory region.
     * @param state_offset Offset to the algorithm's state for this lock.
     * @return LockResult::SUCCESS if the request was submitted successfully.
     *         ERROR_INVALID_PARAM if node_id or lock_id is out of range.
     *         ERROR_SHM_FAILURE if shared memory access failed.
     */
    virtual LockResult acquire_lock(
        NodeId              node_id,
        LockId              lock_id,
        SharedMemoryRegion* shm,
        ShmOffset           state_offset) = 0;

    /**
     * @brief Release the global lock.
     *
     * Called by a node when it no longer needs the global lock. The algorithm
     * should set the node's slot to IDLE and perform any protocol-specific
     * cleanup (e.g., notify the next waiter in a queue, reset CAS flag).
     *
     * The caller must verify that it actually holds the lock before calling
     * this function; releasing a lock that is not owned is undefined behavior.
     *
     * @param node_id      ID of the node releasing the lock.
     * @param lock_id      ID of the lock to release.
     * @param shm          Pointer to the shared memory region.
     * @param state_offset Offset to the algorithm's state for this lock.
     * @return LockResult::SUCCESS if the lock was released.
     *         ERROR_NOT_OWNER if the node does not hold the lock.
     *         ERROR_INVALID_PARAM if parameters are out of range.
     */
    virtual LockResult release_lock(
        NodeId              node_id,
        LockId              lock_id,
        SharedMemoryRegion* shm,
        ShmOffset           state_offset) = 0;

    /**
     * @brief Check which node should be granted the lock next.
     *
     * Called periodically by the lock manager to determine if any node
     * is waiting and should be granted the lock. The algorithm should
     * examine its shared state and apply its scheduling policy (FIFO,
     * round-robin, priority-based, etc.) to select the next node.
     *
     * This function must NOT actually grant the lock; it only returns
     * the node ID to grant. The lock manager will call grant_lock()
     * separately to perform the actual grant operation.
     *
     * @param lock_id      ID of the lock to check.
     * @param shm          Pointer to the shared memory region.
     * @param state_offset Offset to the algorithm's state for this lock.
     * @param node_count   Number of nodes in the system.
     * @return NodeId of the next node to grant, or INVALID_NODE_ID if
     *         no node is waiting or the lock is currently held.
     */
    virtual NodeId check_grant(
        LockId              lock_id,
        SharedMemoryRegion* shm,
        ShmOffset           state_offset,
        uint32_t            node_count) = 0;

    /**
     * @brief Grant the lock to a specific node.
     *
     * Called by the lock manager after check_grant() returns a valid node ID.
     * The algorithm should update the shared state to indicate that the
     * specified node now holds the lock (e.g., set its slot to LOCKED,
     * advance a queue pointer).
     *
     * This is a two-step process (check_grant + grant_lock) to allow the
     * lock manager to perform additional validation or logging between
     * selection and actual grant.
     *
     * @param node_id      ID of the node being granted the lock.
     * @param lock_id      ID of the lock being granted.
     * @param shm          Pointer to the shared memory region.
     * @param state_offset Offset to the algorithm's state for this lock.
     * @return LockResult::SUCCESS if the grant was performed.
     *         ERROR_INVALID_PARAM if the node_id is invalid.
     *         ERROR_BUSY if the lock is already held by another node.
     */
    virtual LockResult grant_lock(
        NodeId              node_id,
        LockId              lock_id,
        SharedMemoryRegion* shm,
        ShmOffset           state_offset) = 0;

    /**
     * @brief Get the size of shared memory state needed by this algorithm.
     *
     * Called during CxlMemoryPool initialization to allocate the correct
     * amount of shared memory for this algorithm's state. The size may
     * depend on the number of nodes (for per-node arrays) and the
     * algorithm's internal data structures.
     *
     * @param node_count Number of nodes that will participate.
     * @return Number of bytes needed in shared memory for the algorithm state.
     */
    virtual size_t get_state_size(uint32_t node_count) const = 0;

    /**
     * @brief Get a human-readable name for this algorithm.
     *
     * Used for logging, debugging, and display in the algorithm registry.
     * The returned pointer must remain valid for the lifetime of the object.
     *
     * @return Null-terminated C string with the algorithm name.
     */
    virtual const char* get_name() const = 0;
};

// ---------------------------------------------------------------------------
// Lock Algorithm Factory and Registry
// ---------------------------------------------------------------------------

/**
 * @brief Factory function type for creating lock algorithm instances.
 *
 * Each registered lock algorithm provides a factory function that the
 * registry uses to create instances on demand. The factory returns a
 * std::unique_ptr to allow polymorphic creation.
 */
using LockAlgorithmFactory = std::unique_ptr<GlobalLockAlgorithm>(*)();

/**
 * @brief Central registry for lock algorithm types.
 *
 * The LockAlgorithmRegistry maintains a mapping from algorithm names to
 * their factory functions. Algorithms are registered at program startup
 * using the REGISTER_LOCK_ALGORITHM macro (which leverages static
 * initialization to auto-register the class).
 *
 * This is a singleton-like class with only static members. It is thread-safe
 * for concurrent registration and creation operations.
 *
 * Example usage:
 * @code
 *   // List all available algorithms
 *   LockAlgorithmRegistry::list_algorithms();
 *
 *   // Create an instance of a specific algorithm
 *   auto lock = LockAlgorithmRegistry::create("cas_mcs");
 *   if (lock) {
 *       lock->initialize(shm, offset, node_count);
 *   }
 * @endcode
 */
class LockAlgorithmRegistry {
public:
    /**
     * @brief Register a new lock algorithm type.
     *
     * Associates the given algorithm name with its factory function.
     * If an algorithm with the same name is already registered, the
     * registration fails and returns false.
     *
     * This function is typically called automatically via the
     * REGISTER_LOCK_ALGORITHM macro during static initialization.
     *
     * @param name    Unique name for the algorithm (used with create()).
     * @param factory Factory function that creates instances of the algorithm.
     * @return true if registration succeeded, false if the name is already taken.
     */
    static bool register_algorithm(const char* name, LockAlgorithmFactory factory);

    /**
     * @brief Create a lock algorithm instance by name.
     *
     * Looks up the algorithm in the registry and invokes its factory function
     * to create a new instance. The caller takes ownership of the returned
     * unique_ptr.
     *
     * @param name Name of the algorithm to create (must match a registered name).
     * @return Unique pointer to the created algorithm, or nullptr if the name
     *         is not found in the registry.
     */
    static std::unique_ptr<GlobalLockAlgorithm> create(const char* name);

    /**
     * @brief Print a list of all registered algorithms to stdout.
     *
     * Useful for debugging and for displaying available options to users.
     * Each line shows the algorithm name and a brief description if available.
     */
    static void list_algorithms();

private:
    // Implementation is hidden to allow future changes to the registry
    // data structure without breaking ABI compatibility.
    class Impl;
};

// ---------------------------------------------------------------------------
// Registration Macro
// ---------------------------------------------------------------------------

/**
 * @brief Automatically register a lock algorithm class at program startup.
 *
 * This macro creates a static variable that calls register_algorithm()
 * during the static initialization phase, before main() is entered.
 * This ensures the algorithm is available for creation without requiring
 * manual registration calls.
 *
 * Usage:
 * @code
 *   // In the algorithm's .cpp file, outside any function:
 *   REGISTER_LOCK_ALGORITHM("cas_mcs", CasMcsLock);
 * @endcode
 *
 * @param name       The unique string name for the algorithm.
 * @param class_name The C++ class name that implements GlobalLockAlgorithm.
 */
#define REGISTER_LOCK_ALGORITHM(name, class_name)                           \
    static bool CXL_LOCK_REG_##class_name =                                 \
        ::cxl_lock::LockAlgorithmRegistry::register_algorithm(              \
            name,                                                           \
            []() -> std::unique_ptr<::cxl_lock::GlobalLockAlgorithm> {      \
                return std::make_unique<class_name>();                      \
            })

} // namespace cxl_lock
