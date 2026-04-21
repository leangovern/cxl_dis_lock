/**
 * @file cas_global_lock.cpp
 * @brief Implementation of the CAS-based global lock algorithm.
 */

#include "cas_global_lock.h"
#include "shared_memory.h"
#include <cstring>
#include <cstdio>

namespace cxl_lock {

// =========================================================================
// Slot offset / address helpers
// =========================================================================

ShmOffset CasGlobalLockAlgorithm::get_slot_offset(LockId lock_id,
                                                   NodeId node_id,
                                                   uint32_t node_count,
                                                   ShmOffset state_offset) {
    return state_offset
         + static_cast<ShmOffset>(lock_id) * node_count * sizeof(uint32_t)
         + static_cast<ShmOffset>(node_id) * sizeof(uint32_t);
}

void* CasGlobalLockAlgorithm::get_slot_addr(LockId lock_id,
                                            NodeId node_id,
                                            uint32_t node_count,
                                            SharedMemoryRegion* shm,
                                            ShmOffset state_offset) {
    return shm->offset_to_ptr(get_slot_offset(lock_id, node_id,
                                              node_count, state_offset));
}

LockSlotState CasGlobalLockAlgorithm::read_slot_state(LockId lock_id,
                                                      NodeId node_id,
                                                      uint32_t node_count,
                                                      SharedMemoryRegion* shm,
                                                      ShmOffset state_offset) {
    void* addr = get_slot_addr(lock_id, node_id, node_count, shm, state_offset);
    uint32_t val = shm->atomic_load<uint32_t>(addr, std::memory_order_seq_cst);
    return static_cast<LockSlotState>(val);
}

void CasGlobalLockAlgorithm::write_slot_state(LockId lock_id,
                                              NodeId node_id,
                                              uint32_t node_count,
                                              SharedMemoryRegion* shm,
                                              ShmOffset state_offset,
                                              LockSlotState state) {
    void* addr = get_slot_addr(lock_id, node_id, node_count, shm, state_offset);
    shm->atomic_store<uint32_t>(addr, static_cast<uint32_t>(state),
                                std::memory_order_seq_cst);
    // Ensure the write is visible to other nodes (CXL memory consistency)
    shm->flush_cache(addr, sizeof(uint32_t));
}

// =========================================================================
// GlobalLockAlgorithm interface
// =========================================================================

LockResult CasGlobalLockAlgorithm::initialize(SharedMemoryRegion* shm,
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

    // Zero out the entire state region (sets all slots to IDLE = 0)
    std::memset(base, 0, total_size);
    shm->flush_cache(base, total_size);
    shm->memory_fence();

    return LockResult::SUCCESS;
}

LockResult CasGlobalLockAlgorithm::acquire_lock(NodeId node_id,
                                                LockId lock_id,
                                                SharedMemoryRegion* shm,
                                                ShmOffset state_offset) {
    if (shm == nullptr) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Step 1: Atomically set this node's slot to WAITING
    write_slot_state(lock_id, node_id, node_count_,
                     shm, state_offset, LockSlotState::WAITING);

    // Step 2: Memory fence to ensure ordering before the lock manager scans
    shm->memory_fence();

    return LockResult::SUCCESS;
}

LockResult CasGlobalLockAlgorithm::release_lock(NodeId node_id,
                                                LockId lock_id,
                                                SharedMemoryRegion* shm,
                                                ShmOffset state_offset) {
    if (shm == nullptr) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Set this node's slot to IDLE; the lock manager will see this and
    // can then grant the lock to the next waiting node.
    write_slot_state(lock_id, node_id, node_count_,
                     shm, state_offset, LockSlotState::IDLE);

    shm->memory_fence();
    return LockResult::SUCCESS;
}

NodeId CasGlobalLockAlgorithm::check_grant(LockId lock_id,
                                           SharedMemoryRegion* shm,
                                           ShmOffset state_offset,
                                           uint32_t node_count) {
    if (shm == nullptr || node_count == 0) {
        return INVALID_NODE_ID;
    }

    // First, check if any node currently holds the lock (LOCKED state).
    // If so, the lock is not available.
    for (NodeId nid = 0; nid < node_count; ++nid) {
        LockSlotState st = read_slot_state(lock_id, nid, node_count,
                                           shm, state_offset);
        if (st == LockSlotState::LOCKED) {
            return INVALID_NODE_ID;  // Lock is already held
        }
    }

    // No node holds the lock. Find the first WAITING node.
    for (NodeId nid = 0; nid < node_count; ++nid) {
        LockSlotState st = read_slot_state(lock_id, nid, node_count,
                                           shm, state_offset);
        if (st == LockSlotState::WAITING) {
            return nid;  // This node should be granted the lock
        }
    }

    // No node is waiting for this lock.
    return INVALID_NODE_ID;
}

LockResult CasGlobalLockAlgorithm::grant_lock(NodeId node_id,
                                              LockId lock_id,
                                              SharedMemoryRegion* shm,
                                              ShmOffset state_offset) {
    if (shm == nullptr || node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Atomically set the node's slot from WAITING to LOCKED.
    // We use compare_exchange to ensure we only grant if the node is
    // still in WAITING state (it might have cancelled).
    void* addr = get_slot_addr(lock_id, node_id, node_count_,
                               shm, state_offset);

    uint32_t expected = static_cast<uint32_t>(LockSlotState::WAITING);
    uint32_t desired  = static_cast<uint32_t>(LockSlotState::LOCKED);

    bool granted = shm->compare_exchange(addr, expected, desired);
    if (!granted) {
        // The slot was not in WAITING state (node may have cancelled)
        return LockResult::ERROR_BUSY;
    }

    shm->flush_cache(addr, sizeof(uint32_t));
    shm->memory_fence();
    return LockResult::SUCCESS;
}

size_t CasGlobalLockAlgorithm::get_state_size(uint32_t node_count) const {
    // Each lock has node_count slots, each slot is one uint32_t.
    return static_cast<size_t>(MAX_LOCK_COUNT) * node_count * sizeof(uint32_t);
}

const char* CasGlobalLockAlgorithm::get_name() const {
    return "CASGlobalLock";
}

} // namespace cxl_lock
