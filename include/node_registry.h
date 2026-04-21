/**
 * @file node_registry.h
 * @brief Node registration and discovery service for the CXL Distributed Lock System.
 *
 * This header provides the NodeRegistry class, which manages node registration
 * in the shared memory pool and enables node discovery across the cluster.
 *
 * Each node has:
 *   - A local configuration (loaded from a JSON file) containing its ID,
 *     name, IP address, thread count, and lock manager role.
 *   - A global entry in shared memory (NodeGlobalInfo) visible to all nodes,
 *     including heartbeat timestamps for failure detection.
 *
 * The NodeRegistry handles:
 *   - Loading local node configuration from JSON files.
 *   - Registering/unregistering nodes in the shared memory pool.
 *   - Periodic heartbeat updates for liveness detection.
 *   - Discovery of other active nodes in the cluster.
 *   - Health checking based on heartbeat timeout.
 *
 * @code
 *   // Example: node registration flow
 *   NodeRegistry registry;
 *   registry.load_local_config("/etc/cxl_lock/node_0.json");
 *   registry.register_in_shared_memory(shm, node_registry_offset);
 *
 *   // In a background thread: update heartbeat periodically
 *   while (running) {
 *       registry.update_heartbeat(shm, node_registry_offset);
 *       std::this_thread::sleep_for(std::chrono::seconds(1));
 *   }
 *
 *   // Discover peers
 *   auto peers = registry.discover_nodes(shm, node_registry_offset, MAX_NODE_COUNT);
 *   for (const auto& peer : peers) {
 *       if (peer.node_id != my_id) {
 *           printf("Found node %u: %s at %s\n",
 *                  peer.node_id, peer.node_name, peer.ip_address);
 *       }
 *   }
 * @endcode
 */
#pragma once

#include "lock_types.h"
#include <string>
#include <vector>

namespace cxl_lock {

// Forward declaration
class SharedMemoryRegion;

// ---------------------------------------------------------------------------
// NodeConfig - Local Configuration
// ---------------------------------------------------------------------------

/**
 * @brief Local node configuration read from a JSON config file.
 *
 * This structure contains the static configuration for a single node,
 * typically read from a JSON file at startup. It does NOT contain
 * dynamic state (like heartbeat timestamps) - that lives in NodeGlobalInfo
 * within shared memory.
 */
struct NodeConfig {
    /**
     * @brief Unique node identifier in the cluster.
     *
     * Must be in the range [0, MAX_NODE_COUNT - 1]. Node 0 is typically
     * the designated lock manager node.
     */
    NodeId node_id;

    /**
     * @brief Human-readable name for the node.
     *
     * Used for logging and debugging purposes. Examples: "compute-01",
     * "storage-node-3", "lock-manager".
     */
    std::string node_name;

    /**
     * @brief IP address of the node for out-of-band management.
     *
     * Used for management plane communication (not for the lock protocol,
     * which uses CXL shared memory). Format: IPv4 dotted-decimal (e.g.,
     * "192.168.1.10").
     */
    std::string ip_address;

    /**
     * @brief Number of compute threads on this node.
     *
     * Used for capacity-aware lock scheduling. Nodes with more threads
     * may receive higher priority for certain workloads.
     */
    uint32_t num_compute_threads;

    /**
     * @brief Whether this node runs the lock manager daemon.
     *
     * Typically only node 0 has this set to true. The lock manager is
     * responsible for granting locks to waiting nodes and managing the
     * global lock table.
     */
    bool is_lock_manager;
};

// ---------------------------------------------------------------------------
// NodeGlobalInfo - Shared Memory Entry
// ---------------------------------------------------------------------------

/**
 * @brief Global information stored in shared memory for a single node.
 *
 * This structure is stored in the node registry region of the CXL shared
 * memory pool. It contains both static information (copied from NodeConfig)
 * and dynamic state (heartbeat timestamps, activity flag) that all nodes
 * can observe.
 *
 * The structure is designed to be POD-compatible for direct storage in
 * shared memory. Fixed-size char arrays are used instead of std::string
 * to ensure deterministic layout across compilers.
 */
struct NodeGlobalInfo {
    /**
     * @brief Node identifier (same as NodeConfig::node_id).
     */
    NodeId node_id;

    /**
     * @brief Human-readable node name.
     *
     * Fixed-size buffer to ensure consistent layout in shared memory.
     * Null-terminated if shorter than the buffer size.
     */
    char node_name[64];

    /**
     * @brief IP address of the node.
     *
     * Fixed-size buffer to ensure consistent layout in shared memory.
     * Null-terminated if shorter than the buffer size.
     */
    char ip_address[64];

    /**
     * @brief Number of compute threads on this node.
     */
    uint32_t num_compute_threads;

    /**
     * @brief Whether this node is currently active in the cluster.
     *
     * Set to true during registration and false during unregistration.
     * Other nodes use this flag (along with the heartbeat) to determine
     * if the node is alive.
     */
    bool is_active;

    /**
     * @brief Timestamp when the node registered (monotonic clock, nanoseconds).
     *
     * Used to calculate node uptime and for debugging registration order.
     */
    uint64_t registration_time;

    /**
     * @brief Timestamp of the most recent heartbeat (monotonic clock, nanoseconds).
     *
     * Updated periodically by the node itself. Other nodes check this
     * timestamp against a timeout threshold to detect node failures.
     *
     * A value of 0 indicates that no heartbeat has been sent yet.
     */
    uint64_t last_heartbeat;
};

// ---------------------------------------------------------------------------
// NodeRegistry
// ---------------------------------------------------------------------------

/**
 * @brief Manages node registration, discovery, and health monitoring.
 *
 * The NodeRegistry is responsible for the lifecycle of a node in the
 * distributed lock system:
 *   1. Load local configuration from a JSON file.
 *   2. Register the node in the shared memory pool (write NodeGlobalInfo).
 *   3. Periodically update the heartbeat timestamp.
 *   4. Discover and monitor other nodes in the cluster.
 *   5. Unregister the node on graceful shutdown.
 *
 * The PIMPL idiom is used to hide implementation details (JSON parsing,
 * clock handling, etc.) from the header.
 */
class NodeRegistry {
public:
    /**
     * @brief Construct an unconfigured NodeRegistry.
     *
     * The registry is not usable until load_local_config() is called.
     */
    NodeRegistry();

    /**
     * @brief Destructor - does NOT automatically unregister the node.
     *
     * The caller must explicitly call unregister() before destruction
     * to ensure proper cleanup in shared memory.
     */
    ~NodeRegistry();

    // Disable copy; each NodeRegistry instance manages one node.
    NodeRegistry(const NodeRegistry&) = delete;
    NodeRegistry& operator=(const NodeRegistry&) = delete;

    // Enable move semantics.
    NodeRegistry(NodeRegistry&& other) noexcept;
    NodeRegistry& operator=(NodeRegistry&& other) noexcept;

    /**
     * @brief Load the local node configuration from a JSON file.
     *
     * Parses the JSON file at the given path and populates the local
     * NodeConfig. The JSON file is expected to have the following structure:
     * @code
     *   {
     *       "node_id": 0,
     *       "node_name": "lock-manager",
     *       "ip_address": "192.168.1.10",
     *       "num_compute_threads": 64,
     *       "is_lock_manager": true
     *   }
     * @endcode
     *
     * @param config_path Filesystem path to the JSON configuration file.
     * @return LockResult::SUCCESS if the config was loaded successfully.
     *         ERROR_INVALID_PARAM if the file path is empty.
     *         ERROR_SHM_FAILURE if the file could not be read or parsed.
     *         ERROR_UNKNOWN for unexpected parse errors.
     */
    LockResult load_local_config(const std::string& config_path);

    /**
     * @brief Get the local node configuration.
     *
     * Returns a const reference to the NodeConfig loaded by
     * load_local_config(). The returned reference remains valid for the
     * lifetime of the NodeRegistry.
     *
     * @return Const reference to the local NodeConfig.
     * @pre load_local_config() must have been called successfully.
     */
    const NodeConfig& get_local_config() const;

    /**
     * @brief Register this node in the shared memory pool.
     *
     * Writes the local NodeConfig into the node registry region of the
     * shared memory pool, setting is_active to true and recording the
     * registration time. The node ID determines the slot in the registry
     * array (slot = node_id).
     *
     * After registration, other nodes can discover this node via
     * discover_nodes().
     *
     * @param shm             Pointer to the shared memory region.
     * @param registry_offset Offset to the beginning of the node registry
     *                        array in shared memory.
     * @return LockResult::SUCCESS if registration succeeded.
     *         ERROR_INVALID_PARAM if the node config is invalid.
     *         ERROR_SHM_FAILURE if shared memory access failed.
     */
    LockResult register_in_shared_memory(
        SharedMemoryRegion* shm,
        ShmOffset           registry_offset);

    /**
     * @brief Unregister this node from the shared memory pool.
     *
     * Sets is_active to false in the node's shared memory entry.
     * This signals to other nodes that this node is leaving the cluster.
     * The heartbeat timestamp is preserved for diagnostic purposes.
     *
     * @param shm             Pointer to the shared memory region.
     * @param registry_offset Offset to the node registry array.
     * @return LockResult::SUCCESS if unregistration succeeded.
     *         ERROR_SHM_FAILURE if shared memory access failed.
     */
    LockResult unregister(
        SharedMemoryRegion* shm,
        ShmOffset           registry_offset);

    /**
     * @brief Update the heartbeat timestamp for this node.
     *
     * Writes the current monotonic clock time to the last_heartbeat field
     * of this node's shared memory entry. This should be called periodically
     * (e.g., every 500ms) to indicate that the node is still alive.
     *
     * @param shm             Pointer to the shared memory region.
     * @param registry_offset Offset to the node registry array.
     * @return LockResult::SUCCESS if the heartbeat was updated.
     *         ERROR_SHM_FAILURE if shared memory access failed.
     */
    LockResult update_heartbeat(
        SharedMemoryRegion* shm,
        ShmOffset           registry_offset);

    /**
     * @brief Get the global info for a specific node.
     *
     * Reads the NodeGlobalInfo entry for the specified node from shared
     * memory. This can be used to check the status of any node in the
     * cluster, including the local node.
     *
     * @param shm             Pointer to the shared memory region.
     * @param registry_offset Offset to the node registry array.
     * @param node_id         ID of the node to query.
     * @param info            Output parameter to receive the node info.
     *                        Must not be null.
     * @return LockResult::SUCCESS if the info was retrieved.
     *         ERROR_INVALID_PARAM if node_id is out of range or info is null.
     *         ERROR_SHM_FAILURE if shared memory access failed.
     */
    LockResult get_node_info(
        SharedMemoryRegion* shm,
        ShmOffset           registry_offset,
        NodeId              node_id,
        NodeGlobalInfo*     info);

    /**
     * @brief Count the number of currently active nodes.
     *
     * Iterates over all node slots in the registry and counts those with
     * is_active set to true. This provides a snapshot of cluster membership.
     *
     * @param shm        Pointer to the shared memory region.
     * @param registry_offset Offset to the node registry array.
     * @param max_nodes  Maximum number of nodes to check (typically MAX_NODE_COUNT).
     * @return Number of active nodes (0 to max_nodes).
     */
    uint32_t get_active_node_count(
        SharedMemoryRegion* shm,
        ShmOffset           registry_offset,
        uint32_t            max_nodes);

    /**
     * @brief Discover all active nodes in the cluster.
     *
     * Scans the node registry and returns a list of NodeGlobalInfo structs
     * for all nodes that have is_active set to true. This is used by nodes
     * to learn about their peers at startup or when cluster membership changes.
     *
     * @param shm             Pointer to the shared memory region.
     * @param registry_offset Offset to the node registry array.
     * @param max_nodes       Maximum number of nodes to scan.
     * @return Vector of NodeGlobalInfo for all active nodes. The vector is
     *         empty if no nodes are active or the shared memory is inaccessible.
     */
    std::vector<NodeGlobalInfo> discover_nodes(
        SharedMemoryRegion* shm,
        ShmOffset           registry_offset,
        uint32_t            max_nodes);

    /**
     * @brief Check if a specific node is alive based on its heartbeat.
     *
     * A node is considered alive if it is active (is_active == true) AND
     * its last heartbeat timestamp is within the specified timeout of the
     * current time. Nodes that fail to update their heartbeat within the
     * timeout are considered dead and may be cleaned up by the lock manager.
     *
     * @param shm             Pointer to the shared memory region.
     * @param registry_offset Offset to the node registry array.
     * @param node_id         ID of the node to check.
     * @param timeout_ms      Heartbeat timeout in milliseconds. A node is
     *                        considered dead if its last heartbeat is older
     *                        than this timeout.
     * @return true if the node is active and its heartbeat is fresh.
     *         false if the node is inactive, its heartbeat is stale, or
     *         the node_id is invalid.
     */
    bool is_node_alive(
        SharedMemoryRegion* shm,
        ShmOffset           registry_offset,
        NodeId              node_id,
        uint64_t            timeout_ms);

private:
    /**
     * @brief Opaque implementation pointer (PIMPL idiom).
     *
     * Hides implementation details (JSON parsing library, clock source,
     * local config storage) from the header.
     */
    class Impl;
    Impl* impl_;
};

} // namespace cxl_lock
