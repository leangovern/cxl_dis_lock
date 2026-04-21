/**
 * @file cas_global_lock.h
 * @brief CAS-based global lock algorithm for CXL distributed locks.
 *
 * The CAS (Compare-And-Swap) global lock uses a simple per-node slot array
 * stored in CXL shared memory. Each node sets its slot to WAITING when it
 * wants the lock, and the lock manager scans slots to find the next node
 * to grant. This is a simple but effective algorithm with O(node_count)
 * scan complexity on the lock manager.
 */

#pragma once

#include "lock_types.h"
#include "lock_interface.h"

namespace cxl_lock {

/**
 * @brief CAS-based global lock algorithm implementation.
 *
 * Shared memory layout for each lock:
 *   [node_0_slot][node_1_slot]...[node_N-1_slot]
 *
 * Where each slot is a uint32_t storing LockSlotState (IDLE/WAITING/LOCKED).
 * The offset for lock_id L, node N is:
 *   state_offset + L * node_count * sizeof(uint32_t) + N * sizeof(uint32_t)
 */

class CasGlobalLockAlgorithm : public GlobalLockAlgorithm {
public:
    CasGlobalLockAlgorithm() = default;
    ~CasGlobalLockAlgorithm() override = default;

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
    uint32_t node_count_ = 0;  ///< Set during initialize(), used for all offset calculations

    // ------------------------------------------------------------------
    // Helpers for computing slot addresses
    // ------------------------------------------------------------------

    /// Compute the shared-memory offset of a node's slot for a given lock.
    static ShmOffset get_slot_offset(LockId lock_id,
                                     NodeId node_id,
                                     uint32_t node_count,
                                     ShmOffset state_offset);

    /// Compute the address of a node's slot for a given lock.
    static void* get_slot_addr(LockId lock_id,
                               NodeId node_id,
                               uint32_t node_count,
                               SharedMemoryRegion* shm,
                               ShmOffset state_offset);

    /// Read the state of a node's slot.
    static LockSlotState read_slot_state(LockId lock_id,
                                         NodeId node_id,
                                         uint32_t node_count,
                                         SharedMemoryRegion* shm,
                                         ShmOffset state_offset);

    /// Write the state of a node's slot and flush cache.
    static void write_slot_state(LockId lock_id,
                                 NodeId node_id,
                                 uint32_t node_count,
                                 SharedMemoryRegion* shm,
                                 ShmOffset state_offset,
                                 LockSlotState state);
};

} // namespace cxl_lock
