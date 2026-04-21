/**
 * @file lock_types.h
 * @brief Common type definitions for the CXL Distributed Lock System.
 *
 * This header provides the fundamental type system used across all components
 * of the CXL distributed lock infrastructure. It defines lock states, node/lock
 * identifiers, shared memory addressing, consistency modes, and operation result
 * codes.
 *
 * All types are designed to be POD-compatible where possible to support
 * direct placement in CXL shared memory regions.
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace cxl_lock {

/**
 * @brief State of a per-node lock slot in the global lock table.
 *
 * Each participating node has a dedicated slot in the global lock entry.
 * The slot transitions through the following lifecycle:
 *   IDLE -> WAITING -> LOCKED -> IDLE
 *
 * These states are stored in CXL shared memory and are visible to all nodes.
 */
enum class LockSlotState : uint32_t {
    /**
     * @brief Slot is idle - the node is not interested in this lock.
     *
     * The lock manager may skip this node when granting the lock.
     */
    IDLE = 0,

    /**
     * @brief Slot is waiting - the node wants to acquire the lock.
     *
     * The node has set its slot to WAITING and is polling for the lock
     * manager to grant it. The lock manager will consider this node
     * when deciding which node to grant the lock to next.
     */
    WAITING = 1,

    /**
     * @brief Slot is locked - the node currently holds the global lock.
     *
     * Only one node can have the LOCKED state for a given lock at any time.
     * The node must release the lock before other nodes can be granted.
     */
    LOCKED = 2
};

/**
 * @brief Unique identifier for a participating server node.
 *
 * Node IDs are assigned during node registration and range from 0 to
 * MAX_NODE_COUNT - 1. Each physical server in the cluster gets one
 * NodeId. The lock manager typically runs on node 0.
 */
using NodeId = uint32_t;

/**
 * @brief Unique identifier for a distributed lock.
 *
 * Lock IDs range from 0 to MAX_LOCK_COUNT - 1. Each lock ID corresponds
 * to one entry in the global lock table in CXL shared memory.
 */
using LockId = uint32_t;

/**
 * @brief Byte offset within the CXL shared memory region.
 *
 * All addresses within the shared memory pool are represented as offsets
 * from the base address of the mapped region. This ensures that the same
 * layout works regardless of where the memory is mapped on each node.
 */
using ShmOffset = uint64_t;

// ---------------------------------------------------------------------------
// Invalid / Sentinel Values
// ---------------------------------------------------------------------------

/**
 * @brief Sentinel value indicating an invalid or unassigned node ID.
 *
 * Used when a function needs to return a NodeId but no valid node exists,
 * such as when the lock manager finds no waiting nodes.
 */
constexpr NodeId INVALID_NODE_ID = static_cast<NodeId>(-1);

/**
 * @brief Sentinel value indicating an invalid or unassigned lock ID.
 */
constexpr LockId INVALID_LOCK_ID = static_cast<LockId>(-1);

/**
 * @brief Sentinel value indicating an invalid shared memory offset.
 */
constexpr ShmOffset INVALID_OFFSET = static_cast<ShmOffset>(-1);

// ---------------------------------------------------------------------------
// Maximum Capacity Constants
// ---------------------------------------------------------------------------

/**
 * @brief Maximum number of nodes that can participate in the lock system.
 *
 * This limit is determined by the shared memory layout (one slot per node
 * per lock) and the target scale of the deployment. Current systems
 * typically have 4-32 nodes per CXL memory pool.
 */
constexpr uint32_t MAX_NODE_COUNT = 64;

/**
 * @brief Maximum number of distributed locks supported.
 *
 * Each lock requires a fixed-size entry in the global lock table.
 * The total shared memory consumption scales linearly with this value.
 * For 64 nodes and 1024 locks: ~64KB for the lock table.
 */
constexpr uint32_t MAX_LOCK_COUNT = 1024;

// ---------------------------------------------------------------------------
// Consistency Mode
// ---------------------------------------------------------------------------

/**
 * @brief Determines how cache coherence is maintained for shared memory.
 *
 * The consistency mode affects the flush/fence behavior of the SharedMemoryRegion.
 * CXL 2.0+ supports hardware cache coherence, but older systems require
 * explicit software cache flushing (clflushopt/clwb + sfence).
 */
enum class ConsistencyMode : uint32_t {
    /**
     * @brief Software-managed consistency using explicit cache flush instructions.
     *
     * The SharedMemoryRegion will issue clflushopt or clwb followed by sfence
     * on every write to ensure that data reaches the CXL memory fabric before
     * subsequent operations observe it. This mode works on all CXL hardware
     * but has higher latency due to the flush overhead.
     */
    SOFTWARE_CONSISTENCY = 0,

    /**
     * @brief Hardware-managed cache coherence (CXL 2.0+).
     *
     * The CXL fabric provides hardware cache coherence, so no explicit flush
     * instructions are needed. This mode has lower latency but requires
     * compatible CXL hardware (e.g., Intel Sapphire Rapids+, AMD EPYC 9004+).
     */
    HARDWARE_CONSISTENCY = 1
};

// ---------------------------------------------------------------------------
// Operation Result Codes
// ---------------------------------------------------------------------------

/**
 * @brief Result code for all lock operations throughout the system.
 *
 * All functions that can fail return a LockResult to indicate success or
 * the specific error condition. Callers should check for SUCCESS (0) and
 * handle error codes appropriately.
 *
 * Negative values indicate errors; SUCCESS (0) is the only non-negative value.
 */
enum class LockResult : int32_t {
    /** @brief Operation completed successfully. */
    SUCCESS = 0,

    /** @brief One or more parameters were invalid (e.g., null pointer, out-of-range ID). */
    ERROR_INVALID_PARAM = -1,

    /** @brief The operation timed out before completion. */
    ERROR_TIMEOUT = -2,

    /** @brief The lock or resource is currently busy and unavailable. */
    ERROR_BUSY = -3,

    /** @brief The caller does not own the lock and cannot release it. */
    ERROR_NOT_OWNER = -4,

    /** @brief A shared memory operation failed (mapping, allocation, access). */
    ERROR_SHM_FAILURE = -5,

    /** @brief The node has not been registered in the shared memory registry. */
    ERROR_NODE_NOT_REGISTERED = -6,

    /** @brief The lock has not been initialized in the global lock table. */
    ERROR_LOCK_NOT_INITIALIZED = -7,

    /** @brief An unexpected or unknown error occurred. */
    ERROR_UNKNOWN = -99
};

} // namespace cxl_lock
