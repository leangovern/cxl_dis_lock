/**
 * @file qspinlock_global_lock.h
 * @brief qspinlock (queued spinlock) global lock algorithm for CXL distributed locks.
 *
 * The qspinlock is a hybrid locking algorithm inspired by Linux's queued spinlock:
 *   - Under low contention, it degenerates to a simple test-and-set lock (fast path).
 *   - Under high contention, it uses an MCS-style queue for FIFO fairness (slow path).
 *
 * Shared memory layout:
 *   [Lock 0 QspinlockEntry][Lock 1 QspinlockEntry]...[Lock MAX_LOCK_COUNT-1]
 *   [Node 0 QspinNode][Node 1 QspinNode]...[Node MAX_NODE_COUNT-1]
 *
 * Each QspinlockEntry is aligned to a 64-byte cache line and contains:
 *   - lock_word: encoded lock state (locked bit, pending bit, MCS tail index)
 *   - owner_node: current lock holder
 *   - pending_tail: head of the MCS queue (next node to grant)
 *   - grant_in_progress: flag to prevent duplicate grants
 *
 * Each QspinNode is aligned to a 64-byte cache line (shared across all locks):
 *   - next_node: MCS chain next pointer
 *   - locked: 0=idle, 1=waiting, 2=granted (LOCKED)
 *   - waiting_lock: which lock this node is waiting on
 *   - mcs_tail: MCS tail tracking
 *
 * Lock word encoding:
 *   Bit 0:     locked     - lock is currently held
 *   Bit 1:     pending    - slow-path contention marker
 *   Bits 2-17: tail_idx   - MCS queue tail (node_id + 1, 0 = empty)
 *   Bits 18-31: reserved  - for future extension
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
 * Aligned to a full cache line (64 bytes) to prevent false sharing
 * between different locks.  The lock_word encodes the locked state,
 * pending flag, and MCS queue tail index.
 */
struct alignas(64) QspinlockEntry {
    uint32_t lock_word;        /**< Encoded lock state:
                                    bit 0: locked,
                                    bit 1: pending,
                                    bits 2-17: tail_idx (node_id+1),
                                    bits 18-31: reserved */
    uint32_t owner_node;       /**< Node ID that currently holds the lock */
    uint32_t pending_tail;     /**< Head of MCS queue (next to grant), INVALID = none */
    uint32_t grant_in_progress;/**< Non-zero when lock manager is granting */
    uint8_t  _cache_pad[48];   /**< Pad to 64 bytes */
};
static_assert(sizeof(QspinlockEntry) == 64,
              "QspinlockEntry must be exactly one cache line");

/**
 * @brief Per-node MCS node stored in CXL shared memory.
 *
 * Each node has one QspinNode that is shared across all locks (a node can
 * only wait on one lock at a time).  Aligned to a full cache line to
 * prevent false sharing between nodes.
 */
struct alignas(64) QspinNode {
    uint32_t next_node;        /**< MCS链表 next 指针 (INVALID_NODE_ID = end) */
    uint32_t locked;           /**< 0=idle, 1=WAITING, 2=LOCKED */
    uint32_t waiting_lock;     /**< 正在等待的 lock_id */
    uint32_t mcs_tail;         /**< MCS tail tracking for this node */
    uint8_t  _cache_pad[48];   /**< Pad to 64 bytes */
};
static_assert(sizeof(QspinNode) == 64,
              "QspinNode must be exactly one cache line");

// ---------------------------------------------------------------------------
// Lock word helpers
// ---------------------------------------------------------------------------

/** @brief Extract the locked bit from a lock_word. */
constexpr inline uint32_t qspin_locked(uint32_t word) {
    return word & 0x1u;
}

/** @brief Extract the pending bit from a lock_word. */
constexpr inline uint32_t qspin_pending(uint32_t word) {
    return (word >> 1) & 0x1u;
}

/** @brief Extract the tail_idx (node_id + 1) from a lock_word. */
constexpr inline uint32_t qspin_tail_idx(uint32_t word) {
    return (word >> 2) & 0xFFFFu;
}

/** @brief Build a lock_word from components (pending/locked preserved, new tail). */
constexpr inline uint32_t qspin_set_tail(uint32_t old_word, uint32_t tail_idx) {
    return (old_word & 0x3u) | (tail_idx << 2);
}

/** @brief Node ID from tail_idx (tail_idx = node_id + 1). */
constexpr inline NodeId qspin_tail_to_node(uint32_t tail_idx) {
    return (tail_idx == 0) ? INVALID_NODE_ID : static_cast<NodeId>(tail_idx - 1);
}

/** @brief tail_idx from node_id. */
constexpr inline uint32_t qspin_node_to_tail(NodeId node_id) {
    return static_cast<uint32_t>(node_id) + 1u;
}

// ---------------------------------------------------------------------------
// Qspinlock global lock algorithm
// ---------------------------------------------------------------------------

/**
 * @brief qspinlock (queued spinlock) global lock algorithm.
 *
 * A hybrid algorithm that combines a test-and-set fast path with an MCS
 * queue slow path for fairness under contention.  Adapted from Linux's
 * qspinlock for CXL distributed shared memory.
 *
 * Fast path: When lock_word == 0, the acquirer CASes it to 1 and owns
 * the lock immediately without lock-manager involvement.
 *
 * Slow path: When the lock is contended, the node joins an MCS queue
 * by linking into the tail of lock_word.  The node spins on its local
 * QspinNode::locked field until the lock manager or the previous holder
 * grants it (sets locked = 2).
 */
class QspinlockGlobalLockAlgorithm : public GlobalLockAlgorithm {
public:
    QspinlockGlobalLockAlgorithm() = default;
    ~QspinlockGlobalLockAlgorithm() override = default;

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
    uint32_t node_count_ = 0;  /**< Set during initialize(), used for offset calc */

    // ------------------------------------------------------------------
    // Offset / address computation helpers
    // ------------------------------------------------------------------

    /// Offset of the QspinlockEntry for lock_id.
    static ShmOffset get_entry_offset(LockId lock_id, ShmOffset state_offset);

    /// Pointer to the QspinlockEntry for lock_id.
    static QspinlockEntry* get_entry(LockId lock_id,
                                     SharedMemoryRegion* shm,
                                     ShmOffset state_offset);

    /// Offset of the QspinNode for node_id (stored after all lock entries).
    static ShmOffset get_node_offset(NodeId node_id,
                                     uint32_t node_count,
                                     ShmOffset state_offset);

    /// Pointer to the QspinNode for node_id.
    static QspinNode* get_node(NodeId node_id,
                               uint32_t node_count,
                               SharedMemoryRegion* shm,
                               ShmOffset state_offset);
};

} // namespace cxl_lock
