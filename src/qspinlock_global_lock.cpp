/**
 * @file qspinlock_global_lock.cpp
 * @brief Implementation of the qspinlock (queued spinlock) global lock algorithm.
 *
 * This implements a hybrid lock that uses a test-and-set fast path under low
 * contention and an MCS queue slow path under high contention.  All lock
 * acquirers (fast or slow path) are part of the MCS queue, ensuring correct
 * lock hand-off via the queue.
 *
 * lock_word encoding:
 *   Bit 0: locked   - 1 if lock is held
 *   Bits 2-17: tail_idx - (node_id + 1) of the MCS queue tail, 0 = empty
 */

#include "qspinlock_global_lock.h"
#include "shared_memory.h"
#include <cstring>

namespace cxl_lock {

// =========================================================================
// Offset / address computation helpers
// =========================================================================

ShmOffset QspinlockGlobalLockAlgorithm::get_entry_offset(LockId lock_id,
                                                         ShmOffset state_offset) {
    return state_offset
         + static_cast<ShmOffset>(lock_id) * sizeof(QspinlockEntry);
}

QspinlockEntry* QspinlockGlobalLockAlgorithm::get_entry(LockId lock_id,
                                                         SharedMemoryRegion* shm,
                                                         ShmOffset state_offset) {
    return static_cast<QspinlockEntry*>(
        shm->offset_to_ptr(get_entry_offset(lock_id, state_offset)));
}

ShmOffset QspinlockGlobalLockAlgorithm::get_node_offset(NodeId node_id,
                                                         uint32_t node_count,
                                                         ShmOffset state_offset) {
    (void)node_count;
    const ShmOffset nodes_start = state_offset
        + static_cast<ShmOffset>(MAX_LOCK_COUNT) * sizeof(QspinlockEntry);
    return nodes_start
         + static_cast<ShmOffset>(node_id) * sizeof(QspinNode);
}

QspinNode* QspinlockGlobalLockAlgorithm::get_node(NodeId node_id,
                                                     uint32_t node_count,
                                                     SharedMemoryRegion* shm,
                                                     ShmOffset state_offset) {
    return static_cast<QspinNode*>(
        shm->offset_to_ptr(get_node_offset(node_id, node_count, state_offset)));
}

// =========================================================================
// GlobalLockAlgorithm interface
// =========================================================================

LockResult QspinlockGlobalLockAlgorithm::initialize(SharedMemoryRegion* shm,
                                                     ShmOffset state_offset,
                                                     uint32_t node_count) {
    if (shm == nullptr) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_count == 0 || node_count > MAX_NODE_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    node_count_ = node_count;

    const size_t total_size = get_state_size(node_count);
    void* base = shm->offset_to_ptr(state_offset);

    // Zero out the entire state region.
    std::memset(base, 0, total_size);

    // Initialize all lock entries.
    for (LockId lid = 0; lid < MAX_LOCK_COUNT; ++lid) {
        QspinlockEntry* entry = get_entry(lid, shm, state_offset);
        entry->lock_word = 0;
        entry->owner_node = INVALID_NODE_ID;
        entry->pending_tail = INVALID_NODE_ID;
        entry->grant_in_progress = 0;
        shm->flush_cache(entry, sizeof(QspinlockEntry));
    }

    // Initialize all node structures.
    for (NodeId nid = 0; nid < node_count; ++nid) {
        QspinNode* node = get_node(nid, node_count, shm, state_offset);
        node->next_node = INVALID_NODE_ID;
        node->locked = 0;
        node->waiting_lock = INVALID_LOCK_ID;
        node->mcs_tail = 0;
        shm->flush_cache(node, sizeof(QspinNode));
    }

    shm->memory_fence();
    return LockResult::SUCCESS;
}

LockResult QspinlockGlobalLockAlgorithm::acquire_lock(NodeId node_id,
                                                         LockId lock_id,
                                                         SharedMemoryRegion* shm,
                                                         ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id >= node_count_) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    QspinNode* node = get_node(node_id, node_count_, shm, state_offset);
    QspinlockEntry* entry = get_entry(lock_id, shm, state_offset);

    // ------------------------------------------------------------------
    // Step 1: Prepare our MCS node
    // ------------------------------------------------------------------
    node->next_node = INVALID_NODE_ID;
    node->locked = 1;            // WAITING state
    node->waiting_lock = lock_id;
    shm->flush_cache(node, sizeof(QspinNode));
    shm->memory_fence();

    // ------------------------------------------------------------------
    // Step 2: Fast path - try to acquire when lock is free (no MCS queue)
    // ------------------------------------------------------------------
    // When lock_word == 0, the lock is free and no one is waiting.
    // We CAS lock_word from 0 to (our_tail | locked=1), making ourselves
    // both the holder and the MCS queue head/tail.
    uint32_t my_tail = qspin_node_to_tail(node_id);
    uint32_t fast_word = (my_tail << 2) | 0x1u;

    if (shm->compare_exchange(&entry->lock_word, 0u, fast_word)) {
        // Fast path success! We are now the sole node in the MCS queue.
        node->locked = 2;        // LOCKED
        entry->owner_node = node_id;
        entry->pending_tail = node_id;  // We are also the queue head
        shm->flush_cache(&entry->owner_node, sizeof(uint32_t));
        shm->flush_cache(&entry->pending_tail, sizeof(uint32_t));
        shm->flush_cache(node, sizeof(QspinNode));
        shm->memory_fence();
        return LockResult::SUCCESS;
    }

    // ------------------------------------------------------------------
    // Step 3: Slow path - join the MCS queue
    // ------------------------------------------------------------------
    // Lock is either held or another waiter is already in the MCS queue.
    // We atomically link ourselves into the tail of the queue.
    while (true) {
        uint32_t old_word = shm->atomic_load<uint32_t>(&entry->lock_word,
                                                        std::memory_order_seq_cst);
        uint32_t old_tail = qspin_tail_idx(old_word);
        uint32_t new_tail = my_tail;
        uint32_t new_word = qspin_set_tail(old_word, new_tail);

        if (shm->compare_exchange(&entry->lock_word, old_word, new_word)) {
            // Successfully linked into the MCS queue.
            if (old_tail == 0) {
                // The previous holder was NOT part of the MCS queue
                // (this should not happen with our fast path, but handle it).
                entry->pending_tail = node_id;
                shm->flush_cache(&entry->pending_tail, sizeof(uint32_t));
            } else {
                // Link after the previous tail node.
                NodeId pred_id = qspin_tail_to_node(old_tail);
                QspinNode* pred = get_node(pred_id, node_count_, shm, state_offset);
                pred->next_node = node_id;
                shm->flush_cache(&pred->next_node, sizeof(uint32_t));
            }
            shm->memory_fence();
            break;
        }
        // CAS failed - retry with updated lock_word.
    }

    return LockResult::SUCCESS;
}

LockResult QspinlockGlobalLockAlgorithm::release_lock(NodeId node_id,
                                                         LockId lock_id,
                                                         SharedMemoryRegion* shm,
                                                         ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id >= node_count_) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    QspinNode* node = get_node(node_id, node_count_, shm, state_offset);
    QspinlockEntry* entry = get_entry(lock_id, shm, state_offset);

    // Verify ownership.
    uint32_t owner = shm->atomic_load<uint32_t>(&entry->owner_node,
                                                 std::memory_order_seq_cst);
    if (owner != static_cast<uint32_t>(node_id)) {
        return LockResult::ERROR_NOT_OWNER;
    }

    // ------------------------------------------------------------------
    // Step 1: Check if we have a successor in the MCS queue
    // ------------------------------------------------------------------
    if (node->next_node == INVALID_NODE_ID) {
        // No successor yet. Try to atomically release the lock and
        // remove ourselves from the MCS queue.
        uint32_t old_word = shm->atomic_load<uint32_t>(&entry->lock_word,
                                                        std::memory_order_seq_cst);
        uint32_t my_tail_idx = qspin_node_to_tail(node_id);
        uint32_t cur_tail = qspin_tail_idx(old_word);

        uint32_t new_word;
        if (cur_tail == my_tail_idx) {
            // We are the tail of the MCS queue and have no successor.
            // Clear the entire lock_word (locked=0, tail=0).
            new_word = 0;
        } else {
            // There are nodes in the queue behind us, but next_node is not
            // set yet (a node is racing to link to us). CAS should fail.
            new_word = old_word;  // Keep unchanged - expect CAS to fail
        }

        if (shm->compare_exchange(&entry->lock_word, old_word, new_word)) {
            if (cur_tail == my_tail_idx) {
                // Successfully released. Clear state.
                entry->pending_tail = INVALID_NODE_ID;
                entry->owner_node = INVALID_NODE_ID;
                shm->flush_cache(entry, sizeof(QspinlockEntry));

                node->next_node = INVALID_NODE_ID;
                node->locked = 0;
                node->waiting_lock = INVALID_LOCK_ID;
                shm->flush_cache(node, sizeof(QspinNode));
                shm->memory_fence();
                return LockResult::SUCCESS;
            }
            // cur_tail != my_tail_idx but CAS succeeded with old_word == new_word?
            // This shouldn't happen, but treat as failure and fall through.
        }

        // CAS failed or we have a successor racing to link.
        // Wait for our next_node to be set by the new node.
        int spin_count = 0;
        while (node->next_node == INVALID_NODE_ID) {
            shm->invalidate_cache(&node->next_node, sizeof(uint32_t));
            if (++spin_count > 1000000) {
                // Prevent infinite spin - but this should not happen
                break;
            }
        }

        if (node->next_node == INVALID_NODE_ID) {
            // Still no successor - force release.
            entry->owner_node = INVALID_NODE_ID;
            shm->flush_cache(&entry->owner_node, sizeof(uint32_t));
            node->locked = 0;
            node->waiting_lock = INVALID_LOCK_ID;
            shm->flush_cache(node, sizeof(QspinNode));
            shm->memory_fence();
            return LockResult::SUCCESS;
        }

        // Fall through to pass the lock to our successor.
    }

    // ------------------------------------------------------------------
    // Step 2: Pass the lock directly to our successor in the MCS queue
    // ------------------------------------------------------------------
    NodeId succ_id = node->next_node;
    QspinNode* succ = get_node(succ_id, node_count_, shm, state_offset);

    // Before waking the successor, update lock_word:
    // - If successor is the tail, lock_word becomes (succ_tail | locked=1)
    // - Otherwise, lock_word keeps successor's tail with locked=1
    uint32_t succ_tail = qspin_node_to_tail(succ_id);
    uint32_t new_lock_word = (succ_tail << 2) | 0x1u;

    // Also advance the pending_tail to the successor (they are now the head).
    entry->pending_tail = succ_id;
    entry->owner_node = succ_id;
    shm->atomic_store(&entry->lock_word, new_lock_word,
                       std::memory_order_seq_cst);
    shm->flush_cache(&entry->lock_word, sizeof(uint32_t));
    shm->flush_cache(&entry->pending_tail, sizeof(uint32_t));
    shm->flush_cache(&entry->owner_node, sizeof(uint32_t));

    // Now wake the successor.
    succ->locked = 2;  // LOCKED
    shm->flush_cache(&succ->locked, sizeof(uint32_t));

    // Reset our own node state.
    node->next_node = INVALID_NODE_ID;
    node->locked = 0;
    node->waiting_lock = INVALID_LOCK_ID;
    shm->flush_cache(node, sizeof(QspinNode));
    shm->memory_fence();

    return LockResult::SUCCESS;
}

NodeId QspinlockGlobalLockAlgorithm::check_grant(LockId lock_id,
                                                    SharedMemoryRegion* shm,
                                                    ShmOffset state_offset,
                                                    uint32_t node_count) {
    if (shm == nullptr || node_count == 0) {
        return INVALID_NODE_ID;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return INVALID_NODE_ID;
    }

    QspinlockEntry* entry = get_entry(lock_id, shm, state_offset);

    // Read the current lock word.
    uint32_t lock_word = shm->atomic_load<uint32_t>(&entry->lock_word,
                                                     std::memory_order_seq_cst);

    // If the lock is currently held (locked bit set), the holder will
    // pass the lock to its successor in the MCS queue via release_lock.
    // No need for the lock manager to intervene.
    if (qspin_locked(lock_word)) {
        return INVALID_NODE_ID;
    }

    // lock_word locked bit is 0. Check if there is an MCS queue.
    uint32_t tail_idx = qspin_tail_idx(lock_word);
    if (tail_idx == 0) {
        // No queue and no holder - lock is free.
        return INVALID_NODE_ID;
    }

    // lock_word has a tail but locked=0. This can happen in a race
    // window during release_lock (between clearing locked bit and
    // waking successor). It can also mean the previous holder released
    // but the next waiter hasn't been woken yet.

    // Check if a grant is already in progress.
    uint32_t gip = shm->atomic_load<uint32_t>(&entry->grant_in_progress,
                                               std::memory_order_seq_cst);
    if (gip != 0) {
        return INVALID_NODE_ID;
    }

    // Get the head of the MCS queue.
    NodeId head = static_cast<NodeId>(
        shm->atomic_load<uint32_t>(&entry->pending_tail,
                                    std::memory_order_seq_cst));
    if (head == INVALID_NODE_ID || head >= node_count) {
        return INVALID_NODE_ID;
    }

    // Check if the head node is waiting for this lock.
    QspinNode* head_node = get_node(head, node_count, shm, state_offset);
    uint32_t head_locked = shm->atomic_load<uint32_t>(&head_node->locked,
                                                       std::memory_order_seq_cst);
    uint32_t head_wait_lock = shm->atomic_load<uint32_t>(&head_node->waiting_lock,
                                                          std::memory_order_seq_cst);
    if (head_locked != 1 || head_wait_lock != lock_id) {
        // Head is already granted (locked==2), not waiting for this lock,
        // or has been reset.
        return INVALID_NODE_ID;
    }

    // Atomically mark grant in progress to prevent duplicate grants.
    shm->atomic_store(&entry->grant_in_progress, 1u,
                       std::memory_order_seq_cst);
    shm->flush_cache(&entry->grant_in_progress, sizeof(uint32_t));

    return head;
}

LockResult QspinlockGlobalLockAlgorithm::grant_lock(NodeId node_id,
                                                       LockId lock_id,
                                                       SharedMemoryRegion* shm,
                                                       ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id >= node_count_) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    QspinlockEntry* entry = get_entry(lock_id, shm, state_offset);
    QspinNode* node = get_node(node_id, node_count_, shm, state_offset);

    // Grant the lock: set the node's state to LOCKED.
    node->locked = 2;
    shm->flush_cache(&node->locked, sizeof(uint32_t));

    // Record the owner and set the locked bit.
    entry->owner_node = node_id;
    uint32_t my_tail = qspin_node_to_tail(node_id);
    uint32_t new_word = (my_tail << 2) | 0x1u;  // Set tail and locked bit
    shm->atomic_store(&entry->lock_word, new_word,
                       std::memory_order_seq_cst);
    shm->flush_cache(&entry->lock_word, sizeof(uint32_t));
    shm->flush_cache(&entry->owner_node, sizeof(uint32_t));

    // Advance the queue head to the next waiter (if any).
    NodeId next_head = node->next_node;
    entry->pending_tail = next_head;
    shm->flush_cache(&entry->pending_tail, sizeof(uint32_t));

    shm->memory_fence();

    // Clear the grant-in-progress flag.
    entry->grant_in_progress = 0;
    shm->flush_cache(&entry->grant_in_progress, sizeof(uint32_t));

    return LockResult::SUCCESS;
}

size_t QspinlockGlobalLockAlgorithm::get_state_size(uint32_t node_count) const {
    size_t lock_entries_size = MAX_LOCK_COUNT * sizeof(QspinlockEntry);
    size_t node_nodes_size = node_count * sizeof(QspinNode);
    return lock_entries_size + node_nodes_size;
}

const char* QspinlockGlobalLockAlgorithm::get_name() const {
    return "QspinlockGlobalLock";
}

// ---------------------------------------------------------------------------
// Algorithm registration
// ---------------------------------------------------------------------------

REGISTER_LOCK_ALGORITHM("qspinlock", QspinlockGlobalLockAlgorithm);

} // namespace cxl_lock
