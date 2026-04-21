/**
 * @file faa_global_lock.h
 * @brief FAA-based global lock algorithm with ticket-lock semantics.
 *
 * The FAA (Fetch-And-Add) global lock provides fair FIFO ordering using
 * a ticket-lock style mechanism:
 *   - Each lock has a next_ticket counter (FAA) and now_serving counter
 *   - Nodes atomically increment next_ticket to get a ticket number
 *   - The lock manager grants locks by advancing now_serving
 *   - Nodes poll until their ticket == now_serving
 *
 * This ensures fairness (no starvation) compared to the CAS-based approach.
 * Each lock entry occupies one cache line for good performance.
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
 * between different locks.
 */
struct alignas(64) FaaGlobalLockEntry {
    uint32_t next_ticket;   ///< Next available ticket number (FAA target)
    uint32_t now_serving;   ///< Current ticket being served / granted
    uint32_t owner_node;    ///< Node ID that currently holds the lock
    uint32_t padding;       ///< Pad to 16 bytes
    // Additional 48 bytes of padding to fill cache line (added by alignment)
    uint8_t  _cache_pad[48];///< Ensure full cache-line occupancy
};
static_assert(sizeof(FaaGlobalLockEntry) == 64,
              "FaaGlobalLockEntry must be exactly one cache line");

/**
 * @brief Per-node waiting ticket storage in shared memory.
 *
 * Each node stores its current ticket number in a dedicated area so the
 * lock manager can find the minimum waiting ticket across all nodes.
 * Aligned to cache line to prevent false sharing.
 */
struct alignas(64) FaaNodeTicket {
    uint32_t ticket;        ///< Ticket number this node is waiting on
    uint32_t active;        ///< Non-zero if this node is waiting
    uint8_t  _pad[56];      ///< Pad to cache line
};
static_assert(sizeof(FaaNodeTicket) == 64,
              "FaaNodeTicket must be exactly one cache line");

// ---------------------------------------------------------------------------
// FAA-based global lock algorithm
// ---------------------------------------------------------------------------

class FaaGlobalLockAlgorithm : public GlobalLockAlgorithm {
public:
    FaaGlobalLockAlgorithm() = default;
    ~FaaGlobalLockAlgorithm() override = default;

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
    // Offset / address computation helpers
    // ------------------------------------------------------------------

    /// Offset of the FaaGlobalLockEntry for lock_id.
    static ShmOffset get_entry_offset(LockId lock_id, ShmOffset state_offset);

    /// Pointer to the FaaGlobalLockEntry for lock_id.
    static FaaGlobalLockEntry* get_entry(LockId lock_id,
                                         SharedMemoryRegion* shm,
                                         ShmOffset state_offset);

    /// Offset of the FaaNodeTicket for node_id (stored after all lock entries).
    static ShmOffset get_ticket_offset(NodeId node_id,
                                       uint32_t node_count,
                                       ShmOffset state_offset);

    /// Pointer to the FaaNodeTicket for node_id.
    static FaaNodeTicket* get_ticket(NodeId node_id,
                                     uint32_t node_count,
                                     SharedMemoryRegion* shm,
                                     ShmOffset state_offset);
};

} // namespace cxl_lock
