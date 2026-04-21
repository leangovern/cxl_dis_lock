/**
 * @file faa_global_lock.cpp
 * @brief Implementation of the FAA-based global lock algorithm (ticket lock).
 */

#include "faa_global_lock.h"
#include "shared_memory.h"
#include <cstring>
#include <limits>
#include <cstdio>

namespace cxl_lock {

// =========================================================================
// Offset / address computation helpers
// =========================================================================

ShmOffset FaaGlobalLockAlgorithm::get_entry_offset(LockId lock_id,
                                                   ShmOffset state_offset) {
    return state_offset
         + static_cast<ShmOffset>(lock_id) * sizeof(FaaGlobalLockEntry);
}

FaaGlobalLockEntry* FaaGlobalLockAlgorithm::get_entry(LockId lock_id,
                                                      SharedMemoryRegion* shm,
                                                      ShmOffset state_offset) {
    return static_cast<FaaGlobalLockEntry*>(
        shm->offset_to_ptr(get_entry_offset(lock_id, state_offset)));
}

ShmOffset FaaGlobalLockAlgorithm::get_ticket_offset(NodeId node_id,
                                                    uint32_t node_count,
                                                    ShmOffset state_offset) {
    // Node tickets are stored after all lock entries.
    const ShmOffset tickets_start = state_offset
        + static_cast<ShmOffset>(MAX_LOCK_COUNT) * sizeof(FaaGlobalLockEntry);
    return tickets_start
         + static_cast<ShmOffset>(node_id) * sizeof(FaaNodeTicket);
}

FaaNodeTicket* FaaGlobalLockAlgorithm::get_ticket(NodeId node_id,
                                                  uint32_t node_count,
                                                  SharedMemoryRegion* shm,
                                                  ShmOffset state_offset) {
    return static_cast<FaaNodeTicket*>(
        shm->offset_to_ptr(get_ticket_offset(node_id, node_count, state_offset)));
}

// =========================================================================
// GlobalLockAlgorithm interface
// =========================================================================

LockResult FaaGlobalLockAlgorithm::initialize(SharedMemoryRegion* shm,
                                              ShmOffset state_offset,
                                              uint32_t node_count) {
    if (shm == nullptr) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_count == 0 || node_count > MAX_NODE_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    node_count_ = node_count;  // Store for use in other methods

    const size_t total_size = get_state_size(node_count);
    void* base = shm->offset_to_ptr(state_offset);

    // Zero out the entire state region.
    std::memset(base, 0, total_size);

    // Initialize all lock entries: next_ticket = 0, now_serving = 0,
    // owner_node = INVALID_NODE_ID.
    for (LockId lid = 0; lid < MAX_LOCK_COUNT; ++lid) {
        FaaGlobalLockEntry* entry = get_entry(lid, shm, state_offset);
        entry->next_ticket = 0;
        entry->now_serving = 0;
        entry->owner_node  = INVALID_NODE_ID;
        entry->padding     = 0;
        shm->flush_cache(entry, sizeof(FaaGlobalLockEntry));
    }

    // Initialize all node tickets to zero (inactive).
    for (NodeId nid = 0; nid < node_count; ++nid) {
        FaaNodeTicket* ticket = get_ticket(nid, node_count, shm, state_offset);
        ticket->ticket = 0;
        ticket->active = 0;
        shm->flush_cache(ticket, sizeof(FaaNodeTicket));
    }

    shm->memory_fence();
    return LockResult::SUCCESS;
}

LockResult FaaGlobalLockAlgorithm::acquire_lock(NodeId node_id,
                                                LockId lock_id,
                                                SharedMemoryRegion* shm,
                                                ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Step 1: Atomically fetch-and-increment next_ticket to get our ticket.
    FaaGlobalLockEntry* entry = get_entry(lock_id, shm, state_offset);
    uint32_t my_ticket = shm->fetch_add(&entry->next_ticket, 1u);

    // Step 2: Record our ticket in the per-node waiting area so the lock
    // manager knows we are waiting and can find our ticket number.
    FaaNodeTicket* nt = get_ticket(node_id, node_count_,
                                   shm, state_offset);
    nt->ticket = my_ticket;
    nt->active = 1;  // Mark as actively waiting
    shm->flush_cache(nt, sizeof(FaaNodeTicket));

    shm->memory_fence();
    return LockResult::SUCCESS;
}

LockResult FaaGlobalLockAlgorithm::release_lock(NodeId node_id,
                                                LockId lock_id,
                                                SharedMemoryRegion* shm,
                                                ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Clear this node's ticket record to indicate we are no longer waiting.
    FaaNodeTicket* nt = get_ticket(node_id, node_count_,
                                   shm, state_offset);
    nt->ticket = 0;
    nt->active = 0;
    shm->flush_cache(nt, sizeof(FaaNodeTicket));

    // Clear the owner_node so check_grant() can find the next waiter.
    FaaGlobalLockEntry* entry = get_entry(lock_id, shm, state_offset);
    shm->atomic_store(&entry->owner_node, INVALID_NODE_ID,
                      std::memory_order_seq_cst);
    shm->flush_cache(&entry->owner_node, sizeof(uint32_t));

    // The lock manager will advance now_serving for the next waiter.
    // We do NOT touch now_serving here — only the lock manager does.
    shm->memory_fence();
    return LockResult::SUCCESS;
}

NodeId FaaGlobalLockAlgorithm::check_grant(LockId lock_id,
                                           SharedMemoryRegion* shm,
                                           ShmOffset state_offset,
                                           uint32_t node_count) {
    if (shm == nullptr || node_count == 0) {
        return INVALID_NODE_ID;
    }

    FaaGlobalLockEntry* entry = get_entry(lock_id, shm, state_offset);

    // If a node already holds this lock, do not grant to another.
    uint32_t owner = shm->atomic_load<uint32_t>(&entry->owner_node,
                                                std::memory_order_seq_cst);
    if (owner != INVALID_NODE_ID) {
        return INVALID_NODE_ID;
    }

    // Find the node with the smallest ticket number that is still waiting.
    uint32_t min_ticket   = std::numeric_limits<uint32_t>::max();
    NodeId   min_node     = INVALID_NODE_ID;

    for (NodeId nid = 0; nid < node_count; ++nid) {
        FaaNodeTicket* nt = get_ticket(nid, node_count, shm, state_offset);
        uint32_t active = shm->atomic_load<uint32_t>(&nt->active,
                                                     std::memory_order_seq_cst);
        if (active) {
            uint32_t t = shm->atomic_load<uint32_t>(&nt->ticket,
                                                    std::memory_order_seq_cst);
            if (t < min_ticket) {
                min_ticket = t;
                min_node   = nid;
            }
        }
    }

    return min_node;  // INVALID_NODE_ID if no one is waiting
}

LockResult FaaGlobalLockAlgorithm::grant_lock(NodeId node_id,
                                              LockId lock_id,
                                              SharedMemoryRegion* shm,
                                              ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    FaaGlobalLockEntry* entry = get_entry(lock_id, shm, state_offset);
    FaaNodeTicket* nt = get_ticket(node_id, node_count_,
                                   shm, state_offset);

    // Read the ticket number this node is waiting for.
    uint32_t grant_ticket = shm->atomic_load<uint32_t>(&nt->ticket,
                                                       std::memory_order_seq_cst);

    // Atomically set owner_node and now_serving.
    // We set now_serving to grant_ticket so the node knows it has the lock
    // (node polls: my_ticket == now_serving).
    shm->atomic_store(&entry->owner_node, static_cast<uint32_t>(node_id),
                      std::memory_order_seq_cst);
    shm->atomic_store(&entry->now_serving, grant_ticket,
                      std::memory_order_seq_cst);

    // NOTE: Do NOT clear nt->active here.  The node polls with
    // (now_serving >= my_ticket && active != 0), so active must remain
    // non-zero until the node actually releases the lock.  The active flag
    // is cleared only in release_lock().

    shm->flush_cache(entry, sizeof(FaaGlobalLockEntry));
    shm->flush_cache(nt, sizeof(FaaNodeTicket));
    shm->memory_fence();

    return LockResult::SUCCESS;
}

size_t FaaGlobalLockAlgorithm::get_state_size(uint32_t node_count) const {
    // Lock entries followed by per-node ticket records.
    size_t lock_entries_size = MAX_LOCK_COUNT * sizeof(FaaGlobalLockEntry);
    size_t node_tickets_size = node_count * sizeof(FaaNodeTicket);
    return lock_entries_size + node_tickets_size;
}

const char* FaaGlobalLockAlgorithm::get_name() const {
    return "FAAGlobalLock";
}

} // namespace cxl_lock
