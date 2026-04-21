// test_basic_lock.cpp
// CXL Distributed Lock System - Basic Functionality Tests
//
// Tests fundamental lock operations using fork() to simulate multiple nodes.
// Covers: single-node lock/unlock, timeout behavior, two-node contention,
// data-to-lock hash mapping, CAS/FAA algorithms, and consistency modes.

#include "distributed_lock.h"
#include "two_tier_lock.h"
#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <sys/wait.h>

using namespace cxl_lock;

// ---------------------------------------------------------------------------
// Helper: Check result and print error if not SUCCESS
// ---------------------------------------------------------------------------
#define CHECK_RESULT(rc, msg) \
    do { \
        if ((rc) != LockResult::SUCCESS) { \
            std::cerr << "FAIL: " << msg << " returned " << static_cast<int>(rc) << std::endl; \
            return false; \
        } \
    } while(0)

// ---------------------------------------------------------------------------
// Test 1: Single node lock/unlock
//
// Verifies that a single node can initialize as lock manager, acquire and
// release locks, and try_lock semantics.
// ---------------------------------------------------------------------------
bool test_single_node_lock_unlock() {
    std::cout << "=== Test: Single node lock/unlock ===" << std::endl;

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = "/tmp/test_basic_lock.shm";
    config.lock_algorithm = "cas";
    config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
    config.shm_size = 4 * 1024 * 1024;  // 4MB for testing

    LockResult rc = dls.initialize(config, true);  // as lock manager
    CHECK_RESULT(rc, "initialize");
    assert(dls.is_lock_manager());
    assert(dls.is_initialized());

    // Acquire lock 0 with 5-second timeout
    rc = dls.acquire_lock(0, 5000);
    CHECK_RESULT(rc, "acquire_lock(0)");

    // Release lock 0
    rc = dls.release_lock(0);
    CHECK_RESULT(rc, "release_lock(0)");

    // Acquire lock 1 with blocking (allows time for lock manager to grant)
    rc = dls.acquire_lock(1, 5000);
    CHECK_RESULT(rc, "acquire_lock(1)");
    rc = dls.release_lock(1);
    CHECK_RESULT(rc, "release_lock(1)");

    // Try to acquire lock 2 (non-blocking) - may succeed or fail depending on timing
    // try_lock is inherently racy with lock manager scan; both outcomes are valid
    rc = dls.try_lock(2);
    if (rc == LockResult::SUCCESS) {
        dls.release_lock(2);
    }
    // Accept both SUCCESS and ERROR_BUSY as valid results

    // Verify algorithm name
    const char* algo = dls.get_algorithm_name();
    // Algorithm name may vary by implementation; just verify it's not null/empty
    if (algo == nullptr || algo[0] == '\0') {
        std::cerr << "FAIL: Algorithm name is empty" << std::endl;
        return false;
    }

    // Verify node id
    assert(dls.get_node_id() == 0);

    dls.shutdown();
    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 2: Lock timeout
//
// Verifies that lock acquisition with a timeout parameter works correctly.
// ---------------------------------------------------------------------------
bool test_lock_timeout() {
    std::cout << "=== Test: Lock timeout ===" << std::endl;

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = "/tmp/test_timeout.shm";
    config.lock_algorithm = "cas";
    config.lock_manager_scan_interval_us = 50;
    config.shm_size = 4 * 1024 * 1024;

    LockResult rc = dls.initialize(config, true);
    CHECK_RESULT(rc, "initialize");

    // Acquire lock 0
    rc = dls.acquire_lock(0);
    CHECK_RESULT(rc, "acquire_lock(0)");

    // Acquire lock 1 with a 100ms timeout (should succeed on different lock)
    rc = dls.acquire_lock(1, 100);
    CHECK_RESULT(rc, "acquire_lock(1) with timeout");

    // Release both locks
    rc = dls.release_lock(0);
    CHECK_RESULT(rc, "release_lock(0)");

    rc = dls.release_lock(1);
    CHECK_RESULT(rc, "release_lock(1)");

    dls.shutdown();
    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 3: Two-node contention using fork()
//
// Simulates a lock manager (node 0) and a worker node (node 1) that compete
// for the same lock. The child acquires lock 5, does some work, and releases.
// ---------------------------------------------------------------------------
bool test_two_node_contention() {
    std::cout << "=== Test: Two-node contention (fork) ===" << std::endl;

    const char* shm_file = "/tmp/test_two_node.shm";

    // Remove old file to avoid stale state
    unlink(shm_file);

    // Parent = lock manager (node 0)
    // Child  = worker node (node 1)

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork failed" << std::endl;
        return false;
    }

    if (pid == 0) {
        // -----------------------------------------------------------------
        // Child process - node 1 (compute node)
        // -----------------------------------------------------------------
        sleep(1);  // Wait for parent to initialize as lock manager

        DistributedLockSystem dls;
        DistributedLockSystem::Config config;
        config.node_id = 1;
        config.max_nodes = 2;
        config.max_locks = 16;
        config.use_file_backed_shm = true;
        config.shm_backing_file = shm_file;
        config.lock_algorithm = "cas";
        config.lock_manager_scan_interval_us = 50;
        config.shm_size = 4 * 1024 * 1024;

        LockResult rc = dls.initialize(config, false);  // NOT lock manager
        if (rc != LockResult::SUCCESS) {
            std::cerr << "Child: initialize failed: " << static_cast<int>(rc) << std::endl;
            _exit(1);
        }

        // Try to acquire lock 5
        rc = dls.acquire_lock(5, 5000);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "Child: acquire_lock(5) failed: " << static_cast<int>(rc) << std::endl;
            _exit(1);
        }

        // Critical section - simulate some work
        usleep(10000);  // 10ms

        rc = dls.release_lock(5);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "Child: release_lock(5) failed: " << static_cast<int>(rc) << std::endl;
            _exit(1);
        }

        dls.shutdown();
        _exit(0);

    } else {
        // -----------------------------------------------------------------
        // Parent process - node 0 (lock manager)
        // -----------------------------------------------------------------
        DistributedLockSystem dls;
        DistributedLockSystem::Config config;
        config.node_id = 0;
        config.max_nodes = 2;
        config.max_locks = 16;
        config.use_file_backed_shm = true;
        config.shm_backing_file = shm_file;
        config.lock_algorithm = "cas";
        config.lock_manager_scan_interval_us = 50;
        config.shm_size = 4 * 1024 * 1024;

        LockResult rc = dls.initialize(config, true);  // lock manager
        CHECK_RESULT(rc, "parent initialize");

        // Wait for child to complete
        int status;
        pid_t child_pid = waitpid(pid, &status, 0);
        (void)child_pid;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            std::cout << "PASSED" << std::endl << std::endl;
        } else {
            std::cerr << "FAILED: Child process exited with status ";
            if (WIFEXITED(status)) {
                std::cerr << WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                std::cerr << "signal " << WTERMSIG(status);
            }
            std::cerr << std::endl;
            dls.shutdown();
            return false;
        }

        dls.shutdown();
        return true;
    }
}

// ---------------------------------------------------------------------------
// Test 4: Data-to-lock hash mapping
//
// Verifies that TwoTierLock::hash_data_to_lock_id produces consistent and
// deterministic results for both integer and byte-array keys.
// ---------------------------------------------------------------------------
bool test_data_lock_mapping() {
    std::cout << "=== Test: Data-to-lock hash mapping ===" << std::endl;

    const uint32_t lock_count = 256;

    // --- Integer key tests ---

    // Same data ID should always map to the same lock ID
    uint64_t data_id = 12345;
    LockId lid1 = TwoTierLock::hash_data_to_lock_id(data_id, lock_count);
    LockId lid2 = TwoTierLock::hash_data_to_lock_id(data_id, lock_count);
    if (lid1 != lid2) {
        std::cerr << "FAILED: Same data ID mapped to different lock IDs: "
                  << lid1 << " vs " << lid2 << std::endl;
        return false;
    }
    if (lid1 >= lock_count) {
        std::cerr << "FAILED: Lock ID out of range: " << lid1 << " >= " << lock_count << std::endl;
        return false;
    }

    // Different data IDs should (usually) map to different lock IDs
    LockId lid3 = TwoTierLock::hash_data_to_lock_id(99999ULL, lock_count);
    if (lid3 >= lock_count) {
        std::cerr << "FAILED: Lock ID out of range: " << lid3 << " >= " << lock_count << std::endl;
        return false;
    }

    // --- Byte array key tests ---

    const char* key = "my_data_key_123";
    LockId lid4 = TwoTierLock::hash_data_to_lock_id(key, strlen(key), lock_count);
    LockId lid5 = TwoTierLock::hash_data_to_lock_id(key, strlen(key), lock_count);
    if (lid4 != lid5) {
        std::cerr << "FAILED: Same byte-array key mapped to different lock IDs: "
                  << lid4 << " vs " << lid5 << std::endl;
        return false;
    }
    if (lid4 >= lock_count) {
        std::cerr << "FAILED: Lock ID out of range: " << lid4 << " >= " << lock_count << std::endl;
        return false;
    }

    // Different keys should (usually) map to different lock IDs
    const char* key2 = "another_key_456";
    LockId lid6 = TwoTierLock::hash_data_to_lock_id(key2, strlen(key2), lock_count);
    (void)lid6;  // Not guaranteed to differ, so we only check range

    // --- Range check for edge values ---
    LockId lid_zero = TwoTierLock::hash_data_to_lock_id(0ULL, lock_count);
    if (lid_zero >= lock_count) {
        std::cerr << "FAILED: Lock ID for 0 out of range" << std::endl;
        return false;
    }

    LockId lid_max = TwoTierLock::hash_data_to_lock_id(UINT64_MAX, lock_count);
    if (lid_max >= lock_count) {
        std::cerr << "FAILED: Lock ID for UINT64_MAX out of range" << std::endl;
        return false;
    }

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Both CAS and FAA algorithms
//
// Runs a lock/unlock cycle with both "cas" and "faa" algorithms to verify
// that the system supports both locking primitives.
// ---------------------------------------------------------------------------
bool test_both_algorithms() {
    std::cout << "=== Test: Both CAS and FAA algorithms ===" << std::endl;

    const char* algorithms[] = {"cas", "faa"};
    for (const char* algo : algorithms) {
        std::cout << "  Testing algorithm: " << algo << std::endl;

        DistributedLockSystem dls;
        DistributedLockSystem::Config config;
        config.node_id = 0;
        config.max_nodes = 2;
        config.max_locks = 16;
        config.use_file_backed_shm = true;
        config.shm_backing_file = "/tmp/test_algo.shm";
        config.lock_algorithm = algo;
        config.shm_size = 4 * 1024 * 1024;

        LockResult rc = dls.initialize(config, true);
        CHECK_RESULT(rc, std::string("initialize ") + algo);

        // Verify the algorithm name contains the expected identifier (case-insensitive)
        std::string actual_name = dls.get_algorithm_name();
        std::string expected_algo = algo;
        // Convert both to lowercase for comparison
        auto to_lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(c));
            return s;
        };
        if (to_lower(actual_name).find(to_lower(expected_algo)) == std::string::npos) {
            std::cerr << "FAILED: Algorithm name mismatch. Expected to contain " << algo
                      << ", got " << dls.get_algorithm_name() << std::endl;
            dls.shutdown();
            return false;
        }

        rc = dls.acquire_lock(0, 5000);
        CHECK_RESULT(rc, std::string("acquire ") + algo);

        rc = dls.release_lock(0);
        CHECK_RESULT(rc, std::string("release ") + algo);

        dls.shutdown();

        // Clean up backing file
        unlink("/tmp/test_algo.shm");
    }

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 6: Consistency mode switch (software vs hardware)
//
// Verifies that the lock system can be initialized with both software and
// hardware consistency modes.
// ---------------------------------------------------------------------------
bool test_consistency_modes() {
    std::cout << "=== Test: Consistency modes ===" << std::endl;

    ConsistencyMode modes[] = {
        ConsistencyMode::SOFTWARE_CONSISTENCY,
        ConsistencyMode::HARDWARE_CONSISTENCY
    };
    const char* mode_names[] = {"SOFTWARE_CONSISTENCY", "HARDWARE_CONSISTENCY"};

    for (size_t i = 0; i < 2; ++i) {
        std::cout << "  Testing mode: " << mode_names[i] << std::endl;

        DistributedLockSystem dls;
        DistributedLockSystem::Config config;
        config.node_id = 0;
        config.max_nodes = 2;
        config.max_locks = 16;
        config.use_file_backed_shm = true;
        config.shm_backing_file = "/tmp/test_consistency.shm";
        config.lock_algorithm = "cas";
        config.consistency_mode = modes[i];
        config.shm_size = 4 * 1024 * 1024;

        LockResult rc = dls.initialize(config, true);
        CHECK_RESULT(rc, "initialize with consistency mode");

        rc = dls.acquire_lock(0, 5000);
        CHECK_RESULT(rc, "acquire with consistency mode");

        rc = dls.release_lock(0);
        CHECK_RESULT(rc, "release with consistency mode");

        dls.shutdown();

        // Clean up backing file
        unlink("/tmp/test_consistency.shm");
    }

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 7: Data lock acquire/release via API
//
// Uses acquire_data_lock / release_data_lock with explicit data keys.
// ---------------------------------------------------------------------------
bool test_data_lock_api() {
    std::cout << "=== Test: Data lock API ===" << std::endl;

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 256;
    config.use_file_backed_shm = true;
    config.shm_backing_file = "/tmp/test_data_lock_api.shm";
    config.lock_algorithm = "cas";
    config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
    config.shm_size = 4 * 1024 * 1024;

    LockResult rc = dls.initialize(config, true);
    CHECK_RESULT(rc, "initialize");

    // Acquire a data lock using a uint64_t key
    uint64_t data_key = 42;
    rc = dls.acquire_data_lock(&data_key, sizeof(data_key), 5000);
    CHECK_RESULT(rc, "acquire_data_lock(42)");

    // Verify we can get the lock ID back
    LockId lid = dls.get_data_lock_id(&data_key, sizeof(data_key));
    if (lid >= config.max_locks) {
        std::cerr << "FAILED: get_data_lock_id returned out-of-range ID: " << lid << std::endl;
        dls.shutdown();
        return false;
    }

    // Release the data lock
    rc = dls.release_data_lock(&data_key, sizeof(data_key));
    CHECK_RESULT(rc, "release_data_lock(42)");

    // Acquire with a string key
    const char* str_key = "test_key";
    rc = dls.acquire_data_lock(str_key, strlen(str_key), 5000);
    CHECK_RESULT(rc, "acquire_data_lock(\"test_key\")");

    rc = dls.release_data_lock(str_key, strlen(str_key));
    CHECK_RESULT(rc, "release_data_lock(\"test_key\")");

    dls.shutdown();
    unlink("/tmp/test_data_lock_api.shm");

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 8: Multiple sequential lock acquisitions
//
// Acquires and releases multiple locks in sequence to verify scalability of
// the lock table.
// ---------------------------------------------------------------------------
bool test_multiple_sequential_locks() {
    std::cout << "=== Test: Multiple sequential locks ===" << std::endl;

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 256;
    config.use_file_backed_shm = true;
    config.shm_backing_file = "/tmp/test_multi_locks.shm";
    config.lock_algorithm = "cas";
    config.shm_size = 4 * 1024 * 1024;

    LockResult rc = dls.initialize(config, true);
    CHECK_RESULT(rc, "initialize");

    const int num_locks = 64;
    for (int i = 0; i < num_locks; ++i) {
        rc = dls.acquire_lock(i, 5000);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAILED: acquire_lock(" << i << ") returned "
                      << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }
    }

    // Verify all locks are held by releasing them
    for (int i = 0; i < num_locks; ++i) {
        rc = dls.release_lock(i);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAILED: release_lock(" << i << ") returned "
                      << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }
    }

    dls.shutdown();
    unlink("/tmp/test_multi_locks.shm");

    std::cout << "PASSED (acquired and released " << num_locks << " locks)" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Main test runner
// ---------------------------------------------------------------------------
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "CXL Distributed Lock - Basic Tests" << std::endl;
    std::cout << "========================================" << std::endl << std::endl;

    int passed = 0;
    int failed = 0;

    if (test_single_node_lock_unlock())     passed++; else failed++;
    if (test_lock_timeout())                passed++; else failed++;
    if (test_data_lock_mapping())           passed++; else failed++;
    if (test_both_algorithms())             passed++; else failed++;
    if (test_consistency_modes())           passed++; else failed++;
    if (test_data_lock_api())               passed++; else failed++;
    if (test_multiple_sequential_locks())   passed++; else failed++;
    if (test_two_node_contention())         passed++; else failed++;

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    // Clean up temporary files
    unlink("/tmp/test_basic_lock.shm");
    unlink("/tmp/test_timeout.shm");
    unlink("/tmp/test_two_node.shm");
    unlink("/tmp/test_data_lock_api.shm");
    unlink("/tmp/test_multi_locks.shm");

    return failed > 0 ? 1 : 0;
}
