/**
 * @file node_registry.cpp
 * @brief Implementation of NodeRegistry for node registration and discovery.
 *
 * This file implements the NodeRegistry class which manages:
 *   - Loading local node configuration from JSON files (manual parsing)
 *   - Registering/unregistering nodes in CXL shared memory
 *   - Periodic heartbeat updates for liveness detection
 *   - Discovery of active nodes in the cluster
 *   - Health checking based on heartbeat timeout
 *
 * The JSON parser is hand-written to avoid external dependencies. It handles
 * the simple key-value format expected for node configuration files.
 */

#include "node_registry.h"
#include "shared_memory.h"
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cctype>

namespace cxl_lock {

// ============================================================================
// Internal Implementation (PIMPL)
// ============================================================================

/**
 * @brief Private implementation data for NodeRegistry.
 *
 * Stores the local node configuration loaded from a JSON file.
 * The PIMPL idiom hides implementation details from the header.
 */
class NodeRegistry::Impl {
public:
    /** Local node configuration loaded from JSON file. */
    NodeConfig local_config;

    /** True if load_local_config() has been called successfully. */
    bool has_local_config = false;

    /**
     * @brief Get the current monotonic time in nanoseconds.
     *
     * Uses std::chrono::steady_clock for monotonic timestamps that are
     * not affected by system clock changes. This is essential for
     * heartbeat timeout calculations.
     *
     * @return Current time in nanoseconds since an unspecified epoch.
     */
    static uint64_t get_monotonic_time_ns() {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        return static_cast<uint64_t>(ns);
    }

    /**
     * @brief Trim leading and trailing whitespace from a string.
     *
     * @param s String to trim (modified in place).
     */
    static void trim(std::string& s) {
        // Trim leading whitespace
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
            ++start;
        }
        if (start > 0) {
            s.erase(0, start);
        }
        // Trim trailing whitespace
        size_t end = s.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
            --end;
        }
        if (end < s.size()) {
            s.erase(end);
        }
    }

    /**
     * @brief Extract the value from a JSON key-value line.
     *
     * Handles integer, string, and boolean values. Expects lines like:
     *   "key": value,
     *   "key": "string_value",
     *   "key": true,
     *
     * @param line The JSON line to parse.
     * @return The extracted value as a string (quotes stripped for strings).
     */
    static std::string extract_json_value(const std::string& line) {
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            return "";
        }

        std::string value = line.substr(colon_pos + 1);
        trim(value);

        // Remove trailing comma if present
        if (!value.empty() && value.back() == ',') {
            value.pop_back();
            trim(value);
        }

        // Remove surrounding quotes for string values
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        return value;
    }

    /**
     * @brief Extract the key from a JSON key-value line.
     *
     * @param line The JSON line to parse.
     * @return The key string (quotes stripped), or empty string if invalid.
     */
    static std::string extract_json_key(const std::string& line) {
        size_t start = line.find('"');
        if (start == std::string::npos) {
            return "";
        }
        size_t end = line.find('"', start + 1);
        if (end == std::string::npos) {
            return "";
        }
        return line.substr(start + 1, end - start - 1);
    }
};

// ============================================================================
// Construction / Destruction / Move
// ============================================================================

/**
 * @brief Construct an unconfigured NodeRegistry.
 *
 * Allocates the private implementation object. The registry is not usable
 * until load_local_config() is called successfully.
 */
NodeRegistry::NodeRegistry() : impl_(new Impl()) {}

/**
 * @brief Destructor.
 *
 * Note: Does NOT automatically unregister the node. The caller must
 * explicitly call unregister() before destruction for proper cleanup.
 */
NodeRegistry::~NodeRegistry() {
    delete impl_;
}

/**
 * @brief Move constructor - transfers ownership.
 *
 * @param other The NodeRegistry to move from.
 */
NodeRegistry::NodeRegistry(NodeRegistry&& other) noexcept
    : impl_(other.impl_) {
    other.impl_ = new Impl();  // Leave other in valid empty state
}

/**
 * @brief Move assignment - transfers ownership.
 *
 * @param other The NodeRegistry to move from.
 * @return Reference to this object.
 */
NodeRegistry& NodeRegistry::operator=(NodeRegistry&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = new Impl();  // Leave other in valid empty state
    }
    return *this;
}

// ============================================================================
// Local Configuration
// ============================================================================

/**
 * @brief Load the local node configuration from a JSON file.
 *
 * Parses a simple JSON file with the following expected structure:
 * @code
 *   {
 *       "node_id": 0,
 *       "node_name": "node0",
 *       "ip_address": "192.168.1.100",
 *       "num_compute_threads": 4,
 *       "is_lock_manager": true
 *   }
 * @endcode
 *
 * The parser handles basic JSON syntax: quoted keys, quoted string values,
 * integer values, and boolean values (true/false).
 *
 * @param config_path Filesystem path to the JSON configuration file.
 * @return LockResult::SUCCESS if the config was loaded successfully.
 *         ERROR_INVALID_PARAM if the file path is empty.
 *         ERROR_SHM_FAILURE if the file could not be opened or read.
 *         ERROR_UNKNOWN for unexpected parse errors.
 */
LockResult NodeRegistry::load_local_config(const std::string& config_path) {
    // Validate the file path
    if (config_path.empty()) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Open the configuration file
    std::ifstream file(config_path);
    if (!file.is_open()) {
        return LockResult::ERROR_SHM_FAILURE;
    }

    // Initialize config with safe defaults
    impl_->local_config = NodeConfig{};
    impl_->local_config.node_id = INVALID_NODE_ID;
    impl_->local_config.num_compute_threads = 0;
    impl_->local_config.is_lock_manager = false;

    // Parse the JSON file line by line
    std::string line;
    while (std::getline(file, line)) {
        Impl::trim(line);
        if (line.empty() || line == "{" || line == "}") {
            continue;  // Skip empty lines and braces
        }

        std::string key = Impl::extract_json_key(line);
        std::string value = Impl::extract_json_value(line);

        if (key.empty() || value.empty()) {
            continue;  // Skip malformed lines
        }

        // Parse known configuration fields
        if (key == "node_id") {
            try {
                impl_->local_config.node_id = static_cast<NodeId>(
                    std::stoul(value));
            } catch (...) {
                return LockResult::ERROR_UNKNOWN;
            }
        } else if (key == "node_name") {
            impl_->local_config.node_name = value;
        } else if (key == "ip_address") {
            impl_->local_config.ip_address = value;
        } else if (key == "num_compute_threads") {
            try {
                impl_->local_config.num_compute_threads = static_cast<uint32_t>(
                    std::stoul(value));
            } catch (...) {
                return LockResult::ERROR_UNKNOWN;
            }
        } else if (key == "is_lock_manager") {
            impl_->local_config.is_lock_manager = (value == "true");
        }
        // Unknown keys are silently ignored for forward compatibility
    }

    // Validate that we got a valid node_id
    if (impl_->local_config.node_id == INVALID_NODE_ID) {
        return LockResult::ERROR_UNKNOWN;
    }

    impl_->has_local_config = true;
    return LockResult::SUCCESS;
}

/**
 * @brief Get the local node configuration.
 *
 * @return Const reference to the NodeConfig loaded by load_local_config().
 * @pre load_local_config() must have been called successfully.
 */
const NodeConfig& NodeRegistry::get_local_config() const {
    return impl_->local_config;
}

// ============================================================================
// Shared Memory Registration
// ============================================================================

/**
 * @brief Register this node in the shared memory pool.
 *
 * Writes the local NodeConfig into the node registry region of shared
 * memory at the slot corresponding to this node's ID. Sets is_active
 * to true and records the registration time. After this call, other
 * nodes can discover this node via discover_nodes().
 *
 * The shared memory layout for the node registry is:
 *   [NodeGlobalInfo slot 0][NodeGlobalInfo slot 1]...[NodeGlobalInfo slot N-1]
 *
 * @param shm             Pointer to the shared memory region.
 * @param registry_offset Offset to the beginning of the node registry array.
 * @return LockResult::SUCCESS if registration succeeded.
 *         ERROR_INVALID_PARAM if the node config is invalid or parameters are null.
 *         ERROR_SHM_FAILURE if shared memory access failed.
 */
LockResult NodeRegistry::register_in_shared_memory(SharedMemoryRegion* shm,
                                                    ShmOffset registry_offset) {
    // Validate parameters
    if (shm == nullptr || !shm->is_valid()) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (!impl_->has_local_config) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Calculate the offset for this node's slot in the registry
    NodeId node_id = impl_->local_config.node_id;
    if (node_id >= MAX_NODE_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    ShmOffset node_offset = registry_offset + node_id * sizeof(NodeGlobalInfo);
    void* node_ptr = shm->offset_to_ptr(node_offset);
    if (node_ptr == nullptr) {
        return LockResult::ERROR_SHM_FAILURE;
    }

    // Build the NodeGlobalInfo structure
    NodeGlobalInfo info;
    std::memset(&info, 0, sizeof(info));
    info.node_id = node_id;
    info.num_compute_threads = impl_->local_config.num_compute_threads;
    info.is_active = true;
    info.registration_time = Impl::get_monotonic_time_ns();
    info.last_heartbeat = info.registration_time;

    // Copy node name (bounded copy to avoid buffer overflow)
    std::strncpy(info.node_name, impl_->local_config.node_name.c_str(),
                 sizeof(info.node_name) - 1);
    info.node_name[sizeof(info.node_name) - 1] = '\0';

    // Copy IP address (bounded copy)
    std::strncpy(info.ip_address, impl_->local_config.ip_address.c_str(),
                 sizeof(info.ip_address) - 1);
    info.ip_address[sizeof(info.ip_address) - 1] = '\0';

    // Write the NodeGlobalInfo to shared memory
    std::memcpy(node_ptr, &info, sizeof(info));

    // Ensure the write is visible to other nodes
    shm->flush_cache(node_ptr, sizeof(info));
    shm->memory_fence();

    return LockResult::SUCCESS;
}

/**
 * @brief Unregister this node from the shared memory pool.
 *
 * Sets is_active to false in the node's shared memory entry. This
 * signals to other nodes that this node is leaving the cluster.
 * The heartbeat timestamp is preserved for diagnostic purposes.
 *
 * @param shm             Pointer to the shared memory region.
 * @param registry_offset Offset to the node registry array.
 * @return LockResult::SUCCESS if unregistration succeeded.
 *         ERROR_INVALID_PARAM if parameters are null/invalid.
 *         ERROR_SHM_FAILURE if shared memory access failed.
 */
LockResult NodeRegistry::unregister(SharedMemoryRegion* shm,
                                     ShmOffset registry_offset) {
    // Validate parameters
    if (shm == nullptr || !shm->is_valid()) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (!impl_->has_local_config) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Calculate the offset for this node's slot
    NodeId node_id = impl_->local_config.node_id;
    if (node_id >= MAX_NODE_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    ShmOffset node_offset = registry_offset + node_id * sizeof(NodeGlobalInfo);
    void* node_ptr = shm->offset_to_ptr(node_offset);
    if (node_ptr == nullptr) {
        return LockResult::ERROR_SHM_FAILURE;
    }

    // Set is_active to false
    NodeGlobalInfo* info = static_cast<NodeGlobalInfo*>(node_ptr);
    info->is_active = false;

    // Ensure the write is visible to other nodes
    shm->flush_cache(&info->is_active, sizeof(info->is_active));
    shm->memory_fence();

    return LockResult::SUCCESS;
}

// ============================================================================
// Heartbeat
// ============================================================================

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
 *         ERROR_INVALID_PARAM if parameters are null/invalid.
 *         ERROR_SHM_FAILURE if shared memory access failed.
 */
LockResult NodeRegistry::update_heartbeat(SharedMemoryRegion* shm,
                                           ShmOffset registry_offset) {
    // Validate parameters
    if (shm == nullptr || !shm->is_valid()) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (!impl_->has_local_config) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Calculate the offset for this node's slot
    NodeId node_id = impl_->local_config.node_id;
    if (node_id >= MAX_NODE_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    ShmOffset node_offset = registry_offset + node_id * sizeof(NodeGlobalInfo);
    void* node_ptr = shm->offset_to_ptr(node_offset);
    if (node_ptr == nullptr) {
        return LockResult::ERROR_SHM_FAILURE;
    }

    // Update the heartbeat timestamp
    NodeGlobalInfo* info = static_cast<NodeGlobalInfo*>(node_ptr);
    uint64_t now = Impl::get_monotonic_time_ns();
    info->last_heartbeat = now;

    // Ensure the write is visible to other nodes
    shm->flush_cache(&info->last_heartbeat, sizeof(info->last_heartbeat));
    shm->memory_fence();

    return LockResult::SUCCESS;
}

// ============================================================================
// Node Queries
// ============================================================================

/**
 * @brief Get the global info for a specific node.
 *
 * Reads the NodeGlobalInfo entry for the specified node from shared memory.
 * Invalidates the cache before reading to ensure fresh data.
 *
 * @param shm             Pointer to the shared memory region.
 * @param registry_offset Offset to the node registry array.
 * @param node_id         ID of the node to query.
 * @param info            Output parameter to receive the node info.
 * @return LockResult::SUCCESS if the info was retrieved.
 *         ERROR_INVALID_PARAM if node_id is out of range or info is null.
 *         ERROR_SHM_FAILURE if shared memory access failed.
 */
LockResult NodeRegistry::get_node_info(SharedMemoryRegion* shm,
                                        ShmOffset registry_offset,
                                        NodeId node_id,
                                        NodeGlobalInfo* info) {
    // Validate parameters
    if (shm == nullptr || !shm->is_valid() || info == nullptr) {
        return LockResult::ERROR_INVALID_PARAM;
    }
    if (node_id >= MAX_NODE_COUNT) {
        return LockResult::ERROR_INVALID_PARAM;
    }

    // Calculate the offset for the requested node's slot
    ShmOffset node_offset = registry_offset + node_id * sizeof(NodeGlobalInfo);
    void* node_ptr = shm->offset_to_ptr(node_offset);
    if (node_ptr == nullptr) {
        return LockResult::ERROR_SHM_FAILURE;
    }

    // Invalidate cache to read fresh data from CXL memory
    shm->invalidate_cache(node_ptr, sizeof(NodeGlobalInfo));

    // Copy the NodeGlobalInfo from shared memory
    std::memcpy(info, node_ptr, sizeof(NodeGlobalInfo));

    return LockResult::SUCCESS;
}

/**
 * @brief Count the number of currently active nodes.
 *
 * Iterates over all node slots in the registry and counts those with
 * is_active set to true. Invalidates cache before reading each entry
 * to ensure fresh data.
 *
 * @param shm             Pointer to the shared memory region.
 * @param registry_offset Offset to the node registry array.
 * @param max_nodes       Maximum number of nodes to check.
 * @return Number of active nodes (0 to max_nodes).
 */
uint32_t NodeRegistry::get_active_node_count(SharedMemoryRegion* shm,
                                               ShmOffset registry_offset,
                                               uint32_t max_nodes) {
    // Validate parameters
    if (shm == nullptr || !shm->is_valid()) {
        return 0;
    }

    uint32_t count = 0;
    uint32_t limit = max_nodes;
    if (limit > MAX_NODE_COUNT) {
        limit = MAX_NODE_COUNT;
    }

    for (uint32_t i = 0; i < limit; ++i) {
        ShmOffset node_offset = registry_offset + i * sizeof(NodeGlobalInfo);
        void* node_ptr = shm->offset_to_ptr(node_offset);
        if (node_ptr == nullptr) {
            continue;
        }

        // Invalidate cache to get fresh data
        shm->invalidate_cache(node_ptr, sizeof(NodeGlobalInfo));

        NodeGlobalInfo* info = static_cast<NodeGlobalInfo*>(node_ptr);
        if (info->is_active) {
            ++count;
        }
    }

    return count;
}

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
 * @return Vector of NodeGlobalInfo for all active nodes. Empty if none.
 */
std::vector<NodeGlobalInfo> NodeRegistry::discover_nodes(SharedMemoryRegion* shm,
                                                           ShmOffset registry_offset,
                                                           uint32_t max_nodes) {
    std::vector<NodeGlobalInfo> result;

    // Validate parameters
    if (shm == nullptr || !shm->is_valid()) {
        return result;
    }

    uint32_t limit = max_nodes;
    if (limit > MAX_NODE_COUNT) {
        limit = MAX_NODE_COUNT;
    }

    result.reserve(limit);  // Pre-allocate for efficiency

    for (uint32_t i = 0; i < limit; ++i) {
        ShmOffset node_offset = registry_offset + i * sizeof(NodeGlobalInfo);
        void* node_ptr = shm->offset_to_ptr(node_offset);
        if (node_ptr == nullptr) {
            continue;
        }

        // Invalidate cache to get fresh data
        shm->invalidate_cache(node_ptr, sizeof(NodeGlobalInfo));

        NodeGlobalInfo info;
        std::memcpy(&info, node_ptr, sizeof(NodeGlobalInfo));

        if (info.is_active) {
            result.push_back(info);
        }
    }

    return result;
}

/**
 * @brief Check if a specific node is alive based on its heartbeat.
 *
 * A node is considered alive if it is active (is_active == true) AND
 * its last heartbeat timestamp is within the specified timeout of the
 * current time. Nodes that fail to update their heartbeat within the
 * timeout are considered dead.
 *
 * @param shm             Pointer to the shared memory region.
 * @param registry_offset Offset to the node registry array.
 * @param node_id         ID of the node to check.
 * @param timeout_ms      Heartbeat timeout in milliseconds.
 * @return true if the node is active and its heartbeat is fresh.
 *         false if the node is inactive, stale, or parameters are invalid.
 */
bool NodeRegistry::is_node_alive(SharedMemoryRegion* shm,
                                  ShmOffset registry_offset,
                                  NodeId node_id,
                                  uint64_t timeout_ms) {
    // Validate parameters
    if (shm == nullptr || !shm->is_valid()) {
        return false;
    }
    if (node_id >= MAX_NODE_COUNT) {
        return false;
    }
    if (timeout_ms == 0) {
        return false;
    }

    // Read the node's global info
    NodeGlobalInfo info;
    LockResult rc = get_node_info(shm, registry_offset, node_id, &info);
    if (rc != LockResult::SUCCESS) {
        return false;
    }

    // Check if the node is active
    if (!info.is_active) {
        return false;
    }

    // Check heartbeat freshness
    // A heartbeat of 0 means no heartbeat has been sent yet
    if (info.last_heartbeat == 0) {
        return false;
    }

    uint64_t now = Impl::get_monotonic_time_ns();
    uint64_t timeout_ns = timeout_ms * 1000000ULL;  // Convert ms to ns

    // Handle clock wraparound (monotonic clock shouldn't wrap in practice,
    // but we handle it defensively)
    if (now < info.last_heartbeat) {
        // Clock went backwards (shouldn't happen with steady_clock, but be safe)
        return true;
    }

    uint64_t elapsed_ns = now - info.last_heartbeat;
    return elapsed_ns < timeout_ns;
}

} // namespace cxl_lock
