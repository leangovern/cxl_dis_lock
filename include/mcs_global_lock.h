/**
 * @file mcs_global_lock.h
 * @brief MCS (Mellor-Crummy Scott) global lock algorithm for CXL distributed locks.
 *
 * The MCS global lock provides fair FIFO ordering using a linked queue in CXL
 * shared memory. Each node enqueues itself into a per-lock queue by atomically
 * swapping the tail pointer. The lock manager grants the lock to the head of
 * the queue; subsequent lock handoffs are performed directly by the releasing
 * node waking its successor, avoiding lock-manager involvement in the common
 * case.
 *
 * Shared memory layout:
 *   [Lock 0 McsLockEntry][Lock 1 McsLockEntry]...[Lock MAX_LOCK_COUNT-1]
 *   [Node 0 McsNode][Node 1 McsNode]...[Node node_count-1]
 *
 * Each McsLockEntry tracks the queue tail, head, and current owner.
 * Each McsNode serves as a queue node for all locks that the corresponding
 * node requests. A node can wait for at most one lock at a time.
 */

#pragma once

#include "lock_types.h"
#include "lock_interface.h"
#include <cstddef>
#include <cstdint>

namespace cxl_lock {

// ---------------------------------------------------------------------------
// Shared memory state layout
// ---------------------------------------------------------------------------

/**
 * @brief Per-lock entry stored in CXL shared memory.
 *
 * Tracks the MCS queue state for a single lock:
 *   - tail_node: the last node to enqueue (INVALID_NODE_ID if queue empty)
 *   - head_node: the first node in queue (next to be granted)
 *   - owner_node: the node currently holding the lock
 *
 * Aligned to a full cache line (64 bytes) to prevent false sharing
 * between different locks.
 */
struct alignas(64) McsLockEntry {
    uint32_t tail_node;    ///< Queue tail (last node to enqueue)
    uint32_t head_node;    ///< Queue head (first node, next to be granted)
    uint32_t owner_node;   ///< Current lock holder (INVALID_NODE_ID if free)
    uint8_t  _pad[52];     ///< Pad to fill cache line
};
static_assert(sizeof(McsLockEntry) == 64,
              "McsLockEntry must be exactly one cache line");

/**
 * @brief Per-node queue node stored in CXL shared memory.
 *
 * Each node has one McsNode that acts as its queue node for all locks.
 * The node can wait for at most one lock at any time (recorded in
 * waiting_lock).
 *
 * The locked field semantics:
 *   0 = idle / granted (lock has been passed to this node)
 *   1 = waiting (node has enqueued and is waiting to be granted)
 *   2 = locked (lock manager has formally granted the lock)
 *
 * Aligned to a full cache line (64 bytes) to prevent false sharing.
 */
struct alignas(64) McsNode {
    uint32_t next_node;     ///< Successor node ID (INVALID_NODE_ID if none)
    uint32_t locked;        ///< 0=idle/granted, 1=waiting, 2=locked
    uint32_t waiting_lock;  ///< Lock ID this node is waiting for
    uint8_t  _pad[52];      ///< Pad to fill cache line
};
static_assert(sizeof(McsNode) == 64,
              "McsNode must be exactly one cache line");

// ---------------------------------------------------------------------------
// MCS-based global lock algorithm
// ---------------------------------------------------------------------------

/**
 * @brief MCS global lock algorithm implementation.
 *
 * Provides fair FIFO lock ordering via an explicit queue in shared memory.
 * Nodes enqueue themselves by CAS-ing the tail pointer; the lock manager
 * grants the lock to the head node. Lock release passes ownership directly
 * to the successor node without lock-manager involvement.
 */
class McsGlobalLockAlgorithm : public GlobalLockAlgorithm {
public:
    McsGlobalLockAlgorithm() = default;
    ~McsGlobalLockAlgorithm() override = default;

    // -- GlobalLockAlgorithm interface --------------------------------------

    LockResult initialize(SharedMemoryRegion* shm,
                          ShmOffset state_offset,
                          uint32_t node_count) override;

    LockResult acquire_lock(NodeId node_id,
                            LockId lock_id,
                            SharedMemoryRegion* shm,
                            ShmOffset state_offset) override;

    LockResult release_lock(NodeId node_id,
                            LockId lock_id,
                            SharedMemoryRegion* shm,
                            ShmOffset state_offset) override;

    NodeId check_grant(LockId lock_id,
                       SharedMemoryRegion* shm,
                       ShmOffset state_offset,
                       uint32_t node_count) override;

    LockResult grant_lock(NodeId node_id,
                          LockId lock_id,
                          SharedMemoryRegion* shm,
                          ShmOffset state_offset) override;

    size_t get_state_size(uint32_t node_count) const override;

    const char* get_name() const override;

private:
    uint32_t node_count_ = 0;  ///< Set during initialize(), used for offset calculations

    // ------------------------------------------------------------------
    // Offset / address computation helpers
    // ------------------------------------------------------------------

    /// Offset of the McsLockEntry for lock_id.
    static ShmOffset get_entry_offset(LockId lock_id, ShmOffset state_offset);

    /// Pointer to the McsLockEntry for lock_id.
    static McsLockEntry* get_entry(LockId lock_id,
                                   SharedMemoryRegion* shm,
                                   ShmOffset state_offset);

    /// Offset of the McsNode for node_id (stored after all lock entries).
    static ShmOffset get_node_offset(NodeId node_id,
                                     uint32_t node_count,
                                     ShmOffset state_offset);

    /// Pointer to the McsNode for node_id.
    static McsNode* get_node(NodeId node_id,
                             uint32_t node_count,
                             SharedMemoryRegion* shm,
                             ShmOffset state_offset);
};

} // namespace cxl_lock
