/**
 * @file mcs_global_lock.cpp
 * @brief Implementation of the MCS (Mellor-Crummy Scott) global lock algorithm.
 *
 * The MCS lock provides fair FIFO ordering by maintaining an explicit queue
 * of waiting nodes in CXL shared memory. Each lock has an McsLockEntry
 * tracking the queue tail, head, and current owner. Each node has an
 * McsNode acting as its queue element.
 *
 * Acquire protocol:
 *   1. Node initializes its McsNode (next=INVALID, locked=1, waiting_lock=set).
 *   2. Node atomically CAS-es the lock's tail from old_tail to its node_id.
 *   3. If old_tail == INVALID (queue empty), node is the first waiter.
 *   4. If old_tail != INVALID, node sets pred->next = node_id and waits.
 *
 * Grant protocol (lock manager):
 *   1. check_grant() returns the head node if the lock is free and head is waiting.
 *   2. grant_lock() sets node->locked = 2 and records the owner.
 *
 * Release protocol:
 *   1. If no successor, CAS tail from node_id to INVALID.
 *   2. If CAS succeeds, queue is empty; clear head and owner.
 *   3. If CAS fails, a new node is enqueuing; spin for next_node.
 *   4. Once successor is known, pass lock by setting succ->locked = 0,
 *      update head and owner to successor.
 *   5. Reset own node state.
 */

#include "mcs_global_lock.h"
#include "shared_memory.h"
#include <cstring>

namespace cxl_lock {

// =========================================================================
// Offset / address computation helpers
// =========================================================================

ShmOffset McsGlobalLockAlgorithm::get_entry_offset(LockId lock_id,
                                                   ShmOffset state_offset) {
    return state_offset
         + static_cast<ShmOffset>(lock_id) * sizeof(McsLockEntry);
}

McsLockEntry* McsGlobalLockAlgorithm::get_entry(LockId lock_id,
                                                SharedMemoryRegion* shm,
                                                ShmOffset state_offset) {
    return static_cast<McsLockEntry*>(
        shm->offset_to_ptr(get_entry_offset(lock_id, state_offset)));
}

ShmOffset McsGlobalLockAlgorithm::get_node_offset(NodeId node_id,
                                                  uint32_t node_count,
                                                  ShmOffset state_offset) {
    (void)node_count;  // node_count may be used in future layout variants
    // Nodes are stored after all lock entries.
    const ShmOffset nodes_start = state_offset
        + static_cast<ShmOffset>(MAX_LOCK_COUNT) * sizeof(McsLockEntry);
    return nodes_start
         + static_cast<ShmOffset>(node_id) * sizeof(McsNode);
}

McsNode* McsGlobalLockAlgorithm::get_node(NodeId node_id,
                                          uint32_t node_count,
                                          SharedMemoryRegion* shm,
                                          ShmOffset state_offset) {
    return static_cast<McsNode*>(
        shm->offset_to_ptr(get_node_offset(node_id, node_count, state_offset)));
}

// =========================================================================
// GlobalLockAlgorithm interface
// =========================================================================

LockResult McsGlobalLockAlgorithm::initialize(SharedMemoryRegion* shm,
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

    // Initialize all lock entries: tail=INVALID, head=INVALID, owner=INVALID.
    for (LockId lid = 0; lid < MAX_LOCK_COUNT; ++lid) {
        McsLockEntry* entry = get_entry(lid, shm, state_offset);
        entry->tail_node  = static_cast<uint32_t>(INVALID_NODE_ID);
        entry->head_node  = static_cast<uint32_t>(INVALID_NODE_ID);
        entry->owner_node = static_cast<uint32_t>(INVALID_NODE_ID);
        shm->flush_cache(entry, sizeof(McsLockEntry));
    }

    // Initialize all node structures to zero (next=0=INVALID_NODE_ID,
    // locked=0, waiting_lock=0). The memset above already cleared them;
    // ensure each node line is flushed.
    for (NodeId nid = 0; nid < node_count; ++nid) {
        McsNode* node = get_node(nid, node_count, shm, state_offset);
        // Node was zeroed by memset; explicitly set waiting_lock to INVALID.
        node->waiting_lock = static_cast<uint32_t>(INVALID_LOCK_ID);
        shm->flush_cache(node, sizeof(McsNode));
    }

    shm->memory_fence();
    return LockResult::SUCCESS;
}

LockResult McsGlobalLockAlgorithm::acquire_lock(NodeId node_id,
                                                LockId lock_id,
                                                SharedMemoryRegion* shm,
                                                ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id >= node_count_) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Step 1: Get this node's McsNode and initialize it.
    McsNode* node = get_node(node_id, node_count_, shm, state_offset);

    node->next_node    = static_cast<uint32_t>(INVALID_NODE_ID);
    node->locked       = 1;  // 1 = waiting
    node->waiting_lock = static_cast<uint32_t>(lock_id);
    shm->flush_cache(node, sizeof(McsNode));

    // Step 2: Get the lock's McsLockEntry.
    McsLockEntry* entry = get_entry(lock_id, shm, state_offset);

    // Step 3: Atomically swap tail from old_tail to node_id.
    // We use compare_exchange to get the old tail value.
    uint32_t old_tail = static_cast<uint32_t>(INVALID_NODE_ID);
    uint32_t my_id    = static_cast<uint32_t>(node_id);

    // Loop to handle spurious CAS failures.
    while (!shm->compare_exchange(&entry->tail_node, old_tail, my_id)) {
        // Reload the current tail and retry.
        old_tail = shm->atomic_load<uint32_t>(&entry->tail_node,
                                               std::memory_order_seq_cst);
        if (old_tail == my_id) {
            // Should not happen with distinct node IDs, but handle safely.
            break;
        }
    }

    // Step 4: If queue was empty, we are the first node.
    if (old_tail == static_cast<uint32_t>(INVALID_NODE_ID)) {
        // Set head_node to ourselves so the lock manager can find us.
        shm->atomic_store(&entry->head_node, my_id,
                          std::memory_order_seq_cst);
        shm->flush_cache(entry, sizeof(McsLockEntry));
    } else {
        // Queue was not empty; link ourselves after the old tail node.
        McsNode* pred = get_node(static_cast<NodeId>(old_tail),
                                 node_count_, shm, state_offset);
        shm->atomic_store(&pred->next_node, my_id,
                          std::memory_order_seq_cst);
        shm->flush_cache(pred, sizeof(McsNode));
    }

    shm->memory_fence();
    return LockResult::SUCCESS;
}

LockResult McsGlobalLockAlgorithm::release_lock(NodeId node_id,
                                                LockId lock_id,
                                                SharedMemoryRegion* shm,
                                                ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id >= node_count_) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Step 1: Get our McsNode and the lock entry.
    McsNode* node = get_node(node_id, node_count_, shm, state_offset);
    McsLockEntry* entry = get_entry(lock_id, shm, state_offset);

    // Verify ownership.
    uint32_t owner = shm->atomic_load<uint32_t>(&entry->owner_node,
                                                 std::memory_order_seq_cst);
    if (owner != static_cast<uint32_t>(node_id)) {
        return LockResult::ERROR_NOT_OWNER;
    }

    // Step 2: Check if we have a successor.
    uint32_t succ = shm->atomic_load<uint32_t>(&node->next_node,
                                                std::memory_order_seq_cst);

    if (succ == static_cast<uint32_t>(INVALID_NODE_ID)) {
        // No successor apparent. Try to CAS tail from us to INVALID,
        // indicating the queue is now empty.
        uint32_t my_id = static_cast<uint32_t>(node_id);
        bool is_last = shm->compare_exchange(&entry->tail_node, my_id,
                                              static_cast<uint32_t>(INVALID_NODE_ID));
        if (is_last) {
            // Queue is now empty. Clear head and owner.
            shm->atomic_store(&entry->head_node,
                              static_cast<uint32_t>(INVALID_NODE_ID),
                              std::memory_order_seq_cst);
            shm->atomic_store(&entry->owner_node,
                              static_cast<uint32_t>(INVALID_NODE_ID),
                              std::memory_order_seq_cst);
            shm->flush_cache(entry, sizeof(McsLockEntry));

            // Reset our own node.
            node->next_node    = static_cast<uint32_t>(INVALID_NODE_ID);
            node->locked       = 0;
            node->waiting_lock = static_cast<uint32_t>(INVALID_LOCK_ID);
            shm->flush_cache(node, sizeof(McsNode));
            shm->memory_fence();
            return LockResult::SUCCESS;
        }

        // CAS failed: a new node is in the process of enqueuing.
        // Spin until it sets our next_node.
        while ((succ = shm->atomic_load<uint32_t>(&node->next_node,
                                                    std::memory_order_seq_cst))
               == static_cast<uint32_t>(INVALID_NODE_ID)) {
            // Busy-wait. On real hardware, a pause/yield could be inserted.
        }
    }

    // Step 3: We have a successor. Pass the lock to it.
    McsNode* succ_node = get_node(static_cast<NodeId>(succ),
                                  node_count_, shm, state_offset);

    // Update head and owner to the successor.
    shm->atomic_store(&entry->head_node, succ,
                      std::memory_order_seq_cst);
    shm->atomic_store(&entry->owner_node, succ,
                      std::memory_order_seq_cst);

    // Wake up successor: locked = 0 means "granted".
    shm->atomic_store(&succ_node->locked, 0u,
                      std::memory_order_seq_cst);

    shm->flush_cache(succ_node, sizeof(McsNode));
    shm->flush_cache(entry, sizeof(McsLockEntry));

    // Step 4: Reset our own node.
    node->next_node    = static_cast<uint32_t>(INVALID_NODE_ID);
    node->locked       = 0;
    node->waiting_lock = static_cast<uint32_t>(INVALID_LOCK_ID);
    shm->flush_cache(node, sizeof(McsNode));
    shm->memory_fence();

    return LockResult::SUCCESS;
}

NodeId McsGlobalLockAlgorithm::check_grant(LockId lock_id,
                                           SharedMemoryRegion* shm,
                                           ShmOffset state_offset,
                                           uint32_t node_count) {
    if (shm == nullptr || node_count == 0) {
        return INVALID_NODE_ID;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return INVALID_NODE_ID;
    }

    McsLockEntry* entry = get_entry(lock_id, shm, state_offset);

    // If a node already holds this lock, do not grant to another.
    uint32_t owner = shm->atomic_load<uint32_t>(&entry->owner_node,
                                                 std::memory_order_seq_cst);
    if (owner != static_cast<uint32_t>(INVALID_NODE_ID)) {
        return INVALID_NODE_ID;
    }

    // Check the head of the queue.
    uint32_t head = shm->atomic_load<uint32_t>(&entry->head_node,
                                                std::memory_order_seq_cst);
    if (head == static_cast<uint32_t>(INVALID_NODE_ID)) {
        return INVALID_NODE_ID;
    }
    if (head >= node_count) {
        return INVALID_NODE_ID;
    }

    // Verify the head node is actually waiting for this lock.
    McsNode* head_node = get_node(static_cast<NodeId>(head),
                                  node_count, shm, state_offset);
    uint32_t locked = shm->atomic_load<uint32_t>(&head_node->locked,
                                                  std::memory_order_seq_cst);
    uint32_t waiting = shm->atomic_load<uint32_t>(&head_node->waiting_lock,
                                                   std::memory_order_seq_cst);

    if (locked == 1u && waiting == static_cast<uint32_t>(lock_id)) {
        return static_cast<NodeId>(head);
    }

    return INVALID_NODE_ID;
}

LockResult McsGlobalLockAlgorithm::grant_lock(NodeId node_id,
                                              LockId lock_id,
                                              SharedMemoryRegion* shm,
                                              ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (lock_id >= MAX_LOCK_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id >= node_count_) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    McsNode* node = get_node(node_id, node_count_, shm, state_offset);
    McsLockEntry* entry = get_entry(lock_id, shm, state_offset);

    // Set the node's state to LOCKED (2).
    shm->atomic_store(&node->locked, 2u,
                      std::memory_order_seq_cst);

    // Record the owner in the lock entry.
    shm->atomic_store(&entry->owner_node,
                      static_cast<uint32_t>(node_id),
                      std::memory_order_seq_cst);

    shm->flush_cache(node, sizeof(McsNode));
    shm->flush_cache(entry, sizeof(McsLockEntry));
    shm->memory_fence();

    return LockResult::SUCCESS;
}

size_t McsGlobalLockAlgorithm::get_state_size(uint32_t node_count) const {
    // Lock entries followed by per-node queue nodes.
    size_t lock_entries_size = MAX_LOCK_COUNT * sizeof(McsLockEntry);
    size_t node_entries_size = node_count * sizeof(McsNode);
    return lock_entries_size + node_entries_size;
}

const char* McsGlobalLockAlgorithm::get_name() const {
    return "MCSGlobalLock";
}

// ---------------------------------------------------------------------------
// Algorithm registration
// ---------------------------------------------------------------------------

REGISTER_LOCK_ALGORITHM("mcs", McsGlobalLockAlgorithm);

} // namespace cxl_lock
