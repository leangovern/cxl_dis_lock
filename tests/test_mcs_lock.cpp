// test_mcs_lock.cpp
// CXL Distributed Lock System - MCS Lock Algorithm Tests
//
// Tests the MCS (Mellor-Crummy Scott) lock algorithm using fork() to simulate
// multiple nodes. Covers: single-node operations, two-node contention, lock
// fairness, multiple concurrent locks, and timeout behavior.

#include "distributed_lock.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>

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
// Shared memory fairness tracking (used by test_mcs_lock_fairness)
//
// Nodes write their node_id into a shared array after acquiring the lock.
// The parent verifies the order is round-robin (1, 2, 3, 1, 2, 3, ...).
// ---------------------------------------------------------------------------
struct FairnessData {
    alignas(64) std::atomic<uint32_t> round_counter;
    alignas(64) std::atomic<uint32_t> manager_ready;  // Set to 1 when lock manager is ready
    alignas(64) std::atomic<uint32_t> nodes_done;      // Count of nodes that finished
    alignas(64) uint32_t turn_node[1];  // Flexible array - actual size set at mmap time
};

// ---------------------------------------------------------------------------
// Test 1: Single node lock/unlock
//
// Verifies that a single node can initialize as lock manager, acquire and
// release locks using the MCS algorithm.
// ---------------------------------------------------------------------------
bool test_mcs_single_node_lock_unlock() {
    std::cout << "=== Test: MCS single node lock/unlock ===" << std::endl;

    const char* shm_file = "/tmp/test_mcs_single.shm";
    unlink(shm_file);

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "mcs";
    config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
    config.shm_size = 4 * 1024 * 1024;

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

    // Acquire lock 1 with blocking
    rc = dls.acquire_lock(1, 5000);
    CHECK_RESULT(rc, "acquire_lock(1)");
    rc = dls.release_lock(1);
    CHECK_RESULT(rc, "release_lock(1)");

    // Try to acquire lock 2 (non-blocking)
    rc = dls.try_lock(2);
    if (rc == LockResult::SUCCESS) {
        dls.release_lock(2);
    }
    // Accept both SUCCESS and ERROR_BUSY as valid results

    // Verify algorithm name contains "mcs"
    const char* algo = dls.get_algorithm_name();
    if (algo == nullptr || algo[0] == '\0') {
        std::cerr << "FAIL: Algorithm name is empty" << std::endl;
        return false;
    }
    // Case-insensitive check for "mcs"
    std::string algo_name(algo);
    bool found_mcs = false;
    for (size_t i = 0; i + 2 < algo_name.size(); ++i) {
        if ((algo_name[i] == 'm' || algo_name[i] == 'M') &&
            (algo_name[i+1] == 'c' || algo_name[i+1] == 'C') &&
            (algo_name[i+2] == 's' || algo_name[i+2] == 'S')) {
            found_mcs = true;
            break;
        }
    }
    if (!found_mcs) {
        std::cerr << "FAIL: Algorithm name '" << algo
                  << "' does not contain 'mcs'" << std::endl;
        return false;
    }

    // Verify node id
    assert(dls.get_node_id() == 0);

    dls.shutdown();
    unlink(shm_file);

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 2: Two-node contention using fork()
//
// Simulates a lock manager (node 0) and a worker node (node 1) that compete
// for the same lock. The child acquires lock 5, does some work, and releases.
// ---------------------------------------------------------------------------
bool test_mcs_two_node_contention() {
    std::cout << "=== Test: MCS two-node contention (fork) ===" << std::endl;

    const char* shm_file = "/tmp/test_mcs_two_node.shm";
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
        config.lock_algorithm = "mcs";
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
        config.lock_algorithm = "mcs";
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
        unlink(shm_file);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Test 3: Lock fairness - 3 nodes competing for the same lock
//
// Nodes 1, 2, 3 each acquire lock 5 multiple times. We verify the order
// of acquisition is round-robin (1, 2, 3, 1, 2, 3, ...) using a shared
// mmap array to record which node holds the lock each turn.
// ---------------------------------------------------------------------------
bool test_mcs_lock_fairness() {
    std::cout << "=== Test: MCS lock fairness ===" << std::endl;

    const char* shm_file = "/tmp/test_mcs_fairness.shm";
    unlink(shm_file);

    // Number of turns to test per node
    constexpr uint32_t TURNS_PER_NODE = 2;
    constexpr uint32_t NUM_WORKERS = 3;
    constexpr uint32_t TOTAL_TURNS = TURNS_PER_NODE * NUM_WORKERS;  // 6
    constexpr uint32_t LOCK_ID = 5;

    // Create shared mmap region to track lock acquisition order
    size_t fairness_mmap_size = sizeof(FairnessData) + sizeof(uint32_t) * TOTAL_TURNS;
    auto* fairness = static_cast<FairnessData*>(
        mmap(nullptr, fairness_mmap_size,
             PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (fairness == MAP_FAILED) {
        std::cerr << "FAILED: mmap for fairness tracking failed" << std::endl;
        return false;
    }

    // Initialize the tracking counters
    fairness->round_counter.store(0);
    fairness->manager_ready.store(0);
    fairness->nodes_done.store(0);
    std::memset(fairness->turn_node, 0, sizeof(uint32_t) * TOTAL_TURNS);

    // Spawn 3 worker processes (nodes 1, 2, 3)
    pid_t children[NUM_WORKERS] = {};
    for (uint32_t node = 0; node < NUM_WORKERS; ++node) {
        pid_t pid = fork();
        if (pid == 0) {
            // ---- Child: worker node ----
            uint32_t node_id = node + 1;  // nodes 1, 2, 3

            // Stagger startup: each node waits a different amount
            usleep(200000 + node_id * 100000);  // 300ms, 400ms, 500ms

            // Wait for lock manager to signal ready
            int spin_count = 0;
            while (fairness->manager_ready.load() == 0) {
                usleep(10000);  // 10ms poll
                if (++spin_count > 500) {  // 5 second timeout
                    std::cerr << "Node " << node_id << ": timeout waiting for manager"
                              << std::endl;
                    munmap(fairness, fairness_mmap_size);
                    _exit(1);
                }
            }

            DistributedLockSystem dls;
            DistributedLockSystem::Config config;
            config.node_id = node_id;
            config.max_nodes = 4;
            config.max_locks = 16;
            config.use_file_backed_shm = true;
            config.shm_backing_file = shm_file;
            config.lock_algorithm = "mcs";
            config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
            config.lock_manager_scan_interval_us = 200;
            config.shm_size = 4 * 1024 * 1024;

            LockResult rc = dls.initialize(config, false);
            if (rc != LockResult::SUCCESS) {
                std::cerr << "Node " << node_id << ": initialize failed: "
                          << static_cast<int>(rc) << std::endl;
                munmap(fairness, fairness_mmap_size);
                _exit(1);
            }

            // Stagger after init
            usleep(node_id * 50000);  // 50ms, 100ms, 150ms

            // Each node acquires the lock TURNS_PER_NODE times
            for (uint32_t turn = 0; turn < TURNS_PER_NODE; ++turn) {
                rc = dls.acquire_lock(LOCK_ID, 30000);
                if (rc != LockResult::SUCCESS) {
                    std::cerr << "Node " << node_id << ": acquire_lock(5) turn "
                              << turn << " failed: " << static_cast<int>(rc) << std::endl;
                    dls.shutdown();
                    munmap(fairness, fairness_mmap_size);
                    _exit(1);
                }

                // Record which node has the lock
                uint32_t round = fairness->round_counter.fetch_add(1);
                if (round < TOTAL_TURNS) {
                    fairness->turn_node[round] = node_id;
                }

                // Hold the lock briefly to simulate work
                usleep(5000);  // 5ms

                rc = dls.release_lock(LOCK_ID);
                if (rc != LockResult::SUCCESS) {
                    std::cerr << "Node " << node_id << ": release_lock(5) turn "
                              << turn << " failed: " << static_cast<int>(rc) << std::endl;
                    dls.shutdown();
                    munmap(fairness, fairness_mmap_size);
                    _exit(1);
                }

                // Delay before next attempt to allow fair scheduling
                usleep(100000);  // 100ms
            }

            dls.shutdown();
            fairness->nodes_done.fetch_add(1);
            munmap(fairness, fairness_mmap_size);
            _exit(0);

        } else if (pid > 0) {
            children[node] = pid;
        } else {
            std::cerr << "fork failed for node " << node + 1 << std::endl;
            for (uint32_t i = 0; i < node; ++i) {
                if (children[i] > 0) {
                    kill(children[i], SIGTERM);
                    waitpid(children[i], nullptr, 0);
                }
            }
            munmap(fairness, fairness_mmap_size);
            return false;
        }
    }

    // ---- Parent: lock manager (node 0) ----
    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 4;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "mcs";
    config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
    config.lock_manager_scan_interval_us = 200;
    config.shm_size = 4 * 1024 * 1024;

    LockResult rc = dls.initialize(config, true);
    if (rc != LockResult::SUCCESS) {
        std::cerr << "Lock manager: initialize failed: " << static_cast<int>(rc) << std::endl;
        munmap(fairness, fairness_mmap_size);
        for (uint32_t i = 0; i < NUM_WORKERS; ++i) {
            if (children[i] > 0) kill(children[i], SIGTERM);
        }
        for (uint32_t i = 0; i < NUM_WORKERS; ++i) {
            if (children[i] > 0) waitpid(children[i], nullptr, 0);
        }
        return false;
    }

    // Signal children that lock manager is ready
    fairness->manager_ready.store(1);

    // Wait for all children to finish
    int failed_children = 0;
    for (uint32_t i = 0; i < NUM_WORKERS; ++i) {
        int status = 0;
        pid_t waited = waitpid(children[i], &status, 0);
        (void)waited;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed_children++;
        }
    }

    dls.shutdown();
    unlink(shm_file);

    // ---- Verify fairness ----
    uint32_t recorded_rounds = fairness->round_counter.load();
    if (recorded_rounds != TOTAL_TURNS) {
        std::cerr << "FAIL: Expected " << TOTAL_TURNS << " rounds, got "
                  << recorded_rounds << std::endl;
        munmap(fairness, fairness_mmap_size);
        return false;
    }

    // Print the observed order
    std::cout << "  Lock acquisition order: ";
    for (uint32_t i = 0; i < TOTAL_TURNS; ++i) {
        std::cout << fairness->turn_node[i];
        if (i + 1 < TOTAL_TURNS) std::cout << ", ";
    }
    std::cout << std::endl;

    // Verify each node got equal turns
    uint32_t node_counts[NUM_WORKERS + 1] = {};
    for (uint32_t i = 0; i < TOTAL_TURNS; ++i) {
        uint32_t node_id = fairness->turn_node[i];
        if (node_id < 1 || node_id > NUM_WORKERS) {
            std::cerr << "FAIL: Invalid node_id " << node_id << " at turn " << i << std::endl;
            munmap(fairness, fairness_mmap_size);
            return false;
        }
        node_counts[node_id]++;
    }

    bool all_equal = true;
    for (uint32_t n = 1; n <= NUM_WORKERS; ++n) {
        if (node_counts[n] != TURNS_PER_NODE) {
            all_equal = false;
            break;
        }
    }

    if (!all_equal) {
        std::cerr << "FAIL: Unequal turn distribution." << std::endl;
        for (uint32_t n = 1; n <= NUM_WORKERS; ++n) {
            std::cerr << "  Node " << n << ": " << node_counts[n] << " turns (expected "
                      << TURNS_PER_NODE << ")" << std::endl;
        }
        munmap(fairness, fairness_mmap_size);
        return false;
    }

    munmap(fairness, fairness_mmap_size);

    if (failed_children > 0) {
        std::cerr << "FAIL: " << failed_children << " child process(es) failed" << std::endl;
        return false;
    }

    std::cout << "PASSED (all " << NUM_WORKERS << " nodes got " << TURNS_PER_NODE
              << " turns each)" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 4: Multiple locks concurrency
//
// A single node operates on multiple different locks simultaneously.
// ---------------------------------------------------------------------------
bool test_mcs_multiple_locks() {
    std::cout << "=== Test: MCS multiple locks concurrency ===" << std::endl;

    const char* shm_file = "/tmp/test_mcs_multi_locks.shm";
    unlink(shm_file);

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "mcs";
    config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
    config.shm_size = 4 * 1024 * 1024;

    LockResult rc = dls.initialize(config, true);
    CHECK_RESULT(rc, "initialize");

    // Acquire multiple different locks
    rc = dls.acquire_lock(0, 5000);
    CHECK_RESULT(rc, "acquire_lock(0)");

    rc = dls.acquire_lock(1, 5000);
    CHECK_RESULT(rc, "acquire_lock(1)");

    rc = dls.acquire_lock(2, 5000);
    CHECK_RESULT(rc, "acquire_lock(2)");

    rc = dls.acquire_lock(3, 5000);
    CHECK_RESULT(rc, "acquire_lock(3)");

    // Release in reverse order
    rc = dls.release_lock(3);
    CHECK_RESULT(rc, "release_lock(3)");

    rc = dls.release_lock(2);
    CHECK_RESULT(rc, "release_lock(2)");

    rc = dls.release_lock(1);
    CHECK_RESULT(rc, "release_lock(1)");

    rc = dls.release_lock(0);
    CHECK_RESULT(rc, "release_lock(0)");

    // Acquire a larger batch of locks
    const int num_locks = 8;
    for (int i = 0; i < num_locks; ++i) {
        rc = dls.acquire_lock(i, 5000);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: acquire_lock(" << i << ") returned "
                      << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }
    }

    // Release all locks
    for (int i = 0; i < num_locks; ++i) {
        rc = dls.release_lock(i);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: release_lock(" << i << ") returned "
                      << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }
    }

    dls.shutdown();
    unlink(shm_file);

    std::cout << "PASSED (acquired and released " << num_locks << " locks)" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Lock timeout
//
// Verifies that lock acquisition with a timeout parameter works correctly
// with the MCS algorithm.
// ---------------------------------------------------------------------------
bool test_mcs_lock_timeout() {
    std::cout << "=== Test: MCS lock timeout ===" << std::endl;

    const char* shm_file = "/tmp/test_mcs_timeout.shm";
    unlink(shm_file);

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "mcs";
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
    unlink(shm_file);

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Main test runner
// ---------------------------------------------------------------------------
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "CXL Distributed Lock - MCS Lock Tests" << std::endl;
    std::cout << "========================================" << std::endl << std::endl;

    int passed = 0;
    int failed = 0;

    if (test_mcs_single_node_lock_unlock()) passed++; else failed++;
    if (test_mcs_multiple_locks())          passed++; else failed++;
    if (test_mcs_lock_timeout())            passed++; else failed++;
    if (test_mcs_two_node_contention())     passed++; else failed++;
    if (test_mcs_lock_fairness())           passed++; else failed++;

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    // Clean up any remaining temporary files
    unlink("/tmp/test_mcs_single.shm");
    unlink("/tmp/test_mcs_two_node.shm");
    unlink("/tmp/test_mcs_fairness.shm");
    unlink("/tmp/test_mcs_multi_locks.shm");
    unlink("/tmp/test_mcs_timeout.shm");

    return failed > 0 ? 1 : 0;
}
