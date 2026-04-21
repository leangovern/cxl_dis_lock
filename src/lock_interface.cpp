/**
 * @file lock_interface.cpp
 * @brief Implementation of LockAlgorithmRegistry for pluggable lock algorithms.
 *
 * This file implements the LockAlgorithmRegistry class which provides a
 * central registration and discovery mechanism for lock algorithm types.
 * Algorithms are registered at program startup using the
 * REGISTER_LOCK_ALGORITHM macro (via static initialization) and can be
 * created by name at runtime.
 *
 * The registry uses the Meyer's singleton pattern for thread-safe static
 * initialization and a std::map for efficient name-to-factory lookups.
 * A mutex protects the internal map for thread-safe concurrent access.
 */

#include "lock_interface.h"
#include "shared_memory.h"
#include <map>
#include <mutex>
#include <iostream>
#include <cstring>

namespace cxl_lock {

// ============================================================================
// Internal Implementation (anonymous namespace for internal linkage)
// ============================================================================

namespace {

/**
 * @brief Internal implementation data for the lock algorithm registry.
 *
 * This standalone struct is used instead of the private nested Impl class
 * from the header to avoid access permission issues. The header declares
 * a private Impl class for ABI compatibility (PIMPL idiom), but the
 * actual implementation can use any internal type.
 *
 * Protected by a mutex for thread-safe registration and creation.
 */
struct LockRegistryData {
    /** Mutex protecting the factories map for thread-safe access. */
    std::mutex mutex;

    /** Map of registered algorithm names to their factory functions. */
    std::map<std::string, LockAlgorithmFactory> factories;
};

/**
 * @brief Get the singleton LockRegistryData instance.
 *
 * Uses Meyer's singleton pattern for lazy, thread-safe initialization
 * (guaranteed by C++11). This ensures the registry exists before any
 * static initialization attempts to register algorithms.
 *
 * @return Reference to the singleton LockRegistryData instance.
 */
LockRegistryData& get_registry_data() {
    static LockRegistryData data;
    return data;
}

} // anonymous namespace

// ============================================================================
// Algorithm Registration
// ============================================================================

/**
 * @brief Register a new lock algorithm type.
 *
 * Associates the given algorithm name with its factory function in the
 * global registry. If an algorithm with the same name is already
 * registered, the registration fails and returns false.
 *
 * This function is typically called automatically via the
 * REGISTER_LOCK_ALGORITHM macro during static initialization, before
 * main() is entered.
 *
 * Thread-safe: Protected by an internal mutex.
 *
 * @param name    Unique name for the algorithm (used with create()).
 * @param factory Factory function that creates instances of the algorithm.
 * @return true if registration succeeded, false if the name already exists.
 */
bool LockAlgorithmRegistry::register_algorithm(const char* name,
                                                LockAlgorithmFactory factory) {
    // Validate parameters
    if (name == nullptr || name[0] == '\0' || factory == nullptr) {
        return false;
    }

    auto& data = get_registry_data();
    std::lock_guard<std::mutex> lock(data.mutex);

    std::string key(name);

    // Check if the name is already registered
    if (data.factories.count(key)) {
        return false;  // Already registered - prevent overwriting
    }

    // Register the new algorithm
    data.factories[key] = factory;
    return true;
}

// ============================================================================
// Algorithm Creation
// ============================================================================

/**
 * @brief Create a lock algorithm instance by name.
 *
 * Looks up the algorithm in the registry and invokes its factory function
 * to create a new instance. The caller takes ownership of the returned
 * unique_ptr.
 *
 * Thread-safe: Protected by an internal mutex.
 *
 * @param name Name of the algorithm to create (must match a registered name).
 * @return Unique pointer to the created algorithm, or nullptr if the name
 *         is not found in the registry or if name is null/empty.
 */
std::unique_ptr<GlobalLockAlgorithm> LockAlgorithmRegistry::create(const char* name) {
    // Validate parameter
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    auto& data = get_registry_data();
    std::lock_guard<std::mutex> lock(data.mutex);

    auto it = data.factories.find(name);
    if (it == data.factories.end()) {
        return nullptr;  // Algorithm not found
    }

    // Invoke the factory function to create the algorithm instance
    return it->second();
}

// ============================================================================
// Algorithm Discovery
// ============================================================================

/**
 * @brief Print a list of all registered algorithms to stdout.
 *
 * Useful for debugging and for displaying available options to users.
 * Each line shows the algorithm's registered name and the human-readable
 * name returned by the algorithm's get_name() method.
 *
 * Thread-safe: Protected by an internal mutex.
 */
void LockAlgorithmRegistry::list_algorithms() {
    auto& data = get_registry_data();
    std::lock_guard<std::mutex> lock(data.mutex);

    std::cout << "Registered lock algorithms:" << std::endl;

    if (data.factories.empty()) {
        std::cout << "  (none)" << std::endl;
        return;
    }

    for (const auto& [name, factory] : data.factories) {
        // Create a temporary instance to get the algorithm's display name
        auto algo = factory();
        std::cout << "  - " << name;
        if (algo) {
            std::cout << " (" << algo->get_name() << ")";
        } else {
            std::cout << " (factory returned null)";
        }
        std::cout << std::endl;
    }
}

} // namespace cxl_lock
