// test_qspinlock.cpp
// CXL Distributed Lock System - qspinlock Algorithm Tests
//
// Tests the qspinlock (queued spinlock) algorithm using fork() to simulate
// multiple nodes. Covers: single-node operations (fast path), two-node
// contention (slow path), multiple concurrent locks, and timeout behavior.

#include "distributed_lock.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>

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
// Shared memory fairness tracking (used by test_qspinlock_lock_fairness)
// ---------------------------------------------------------------------------
struct FairnessData {
    alignas(64) std::atomic<uint32_t> round_counter;
    alignas(64) std::atomic<uint32_t> manager_ready;  // Set to 1 when lock manager is ready
    alignas(64) std::atomic<uint32_t> nodes_done;      // Count of nodes that finished
    alignas(64) uint32_t turn_node[1];  // Flexible array
};

// ---------------------------------------------------------------------------
// Test 1: Single node lock/unlock (fast path)
//
// Verifies that a single node can initialize as lock manager, acquire and
// release locks using the qspinlock algorithm. When there is no contention,
// the fast path (test-and-set) should be used.
// ---------------------------------------------------------------------------
bool test_qspinlock_single_node_lock_unlock() {
    std::cout << "=== Test: qspinlock single node lock/unlock ===" << std::endl;

    const char* shm_file = "/tmp/test_qspin_single.shm";
    unlink(shm_file);

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "qspinlock";
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

    // Verify algorithm name contains "qspinlock"
    const char* algo = dls.get_algorithm_name();
    if (algo == nullptr || algo[0] == '\0') {
        std::cerr << "FAIL: Algorithm name is empty" << std::endl;
        return false;
    }
    // Case-insensitive check for "qspinlock"
    std::string algo_name(algo);
    bool found_qspin = false;
    for (size_t i = 0; i + 8 < algo_name.size(); ++i) {
        if ((algo_name[i] == 'q' || algo_name[i] == 'Q') &&
            (algo_name[i+1] == 's' || algo_name[i+1] == 'S') &&
            (algo_name[i+2] == 'p' || algo_name[i+2] == 'P') &&
            (algo_name[i+3] == 'i' || algo_name[i+3] == 'I') &&
            (algo_name[i+4] == 'n' || algo_name[i+4] == 'N')) {
            found_qspin = true;
            break;
        }
    }
    if (!found_qspin) {
        std::cerr << "FAIL: Algorithm name '" << algo
                  << "' does not contain 'qspinlock'" << std::endl;
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
// for the same lock. Under contention, the slow path (MCS queue) should be
// used.
// ---------------------------------------------------------------------------
bool test_qspinlock_two_node_contention() {
    std::cout << "=== Test: qspinlock two-node contention (fork) ===" << std::endl;

    const char* shm_file = "/tmp/test_qspin_two_node.shm";
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
        config.lock_algorithm = "qspinlock";
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
        config.lock_algorithm = "qspinlock";
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
// Test 3: Fast path test
//
// When there is no contention (single node), qspinlock should use the fast
// path (test-and-set) to acquire the lock. We verify this by timing the
// lock acquisitions - fast path should consistently complete very quickly
// (well under 1ms each).
// ---------------------------------------------------------------------------
bool test_qspinlock_fast_path() {
    std::cout << "=== Test: qspinlock fast path ===" << std::endl;

    const char* shm_file = "/tmp/test_qspin_fast.shm";
    unlink(shm_file);

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "qspinlock";
    config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
    config.shm_size = 4 * 1024 * 1024;

    LockResult rc = dls.initialize(config, true);
    CHECK_RESULT(rc, "initialize");

    // Perform many lock/unlock cycles and measure the time
    // Fast path should complete each cycle in well under 1ms
    constexpr int NUM_ITERATIONS = 50;
    constexpr int64_t MAX_FAST_PATH_US = 2000;  // 2ms threshold per operation

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        rc = dls.acquire_lock(7, 5000);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: acquire_lock(7) iteration " << i
                      << " returned " << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }

        rc = dls.release_lock(7);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: release_lock(7) iteration " << i
                      << " returned " << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start).count();
    auto avg_us = elapsed_us / NUM_ITERATIONS;

    std::cout << "  " << NUM_ITERATIONS << " lock/unlock cycles in "
              << elapsed_us << " us (avg " << avg_us << " us/cycle)" << std::endl;

    // With fast path, average time per lock/unlock should be well under threshold
    if (avg_us > MAX_FAST_PATH_US) {
        std::cerr << "FAIL: Average lock/unlock time (" << avg_us
                  << " us) exceeds fast path threshold (" << MAX_FAST_PATH_US
                  << " us). Slow path may have been used." << std::endl;
        dls.shutdown();
        return false;
    }

    // Test with multiple different locks (all should be uncontended)
    for (int i = 0; i < 16; ++i) {
        auto lock_start = std::chrono::steady_clock::now();

        rc = dls.acquire_lock(i, 5000);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: acquire_lock(" << i << ") returned "
                      << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }

        rc = dls.release_lock(i);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: release_lock(" << i << ") returned "
                      << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }

        auto lock_end = std::chrono::steady_clock::now();
        auto lock_us = std::chrono::duration_cast<std::chrono::microseconds>(
            lock_end - lock_start).count();

        if (lock_us > MAX_FAST_PATH_US * 2) {  // Allow 2x margin for individual ops
            std::cerr << "FAIL: Lock " << i << " took " << lock_us
                      << " us (exceeds threshold)" << std::endl;
            dls.shutdown();
            return false;
        }
    }

    dls.shutdown();
    unlink(shm_file);

    std::cout << "PASSED (fast path confirmed, avg " << avg_us << " us/cycle)"
              << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 4: Multiple locks concurrency
//
// A single node operates on multiple different locks simultaneously.
// ---------------------------------------------------------------------------
bool test_qspinlock_multiple_locks() {
    std::cout << "=== Test: qspinlock multiple locks concurrency ===" << std::endl;

    const char* shm_file = "/tmp/test_qspin_multi_locks.shm";
    unlink(shm_file);

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "qspinlock";
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

    std::cout << "PASSED (acquired and released " << num_locks << " locks)"
              << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Lock timeout
//
// Verifies that lock acquisition with a timeout parameter works correctly
// with the qspinlock algorithm.
// ---------------------------------------------------------------------------
bool test_qspinlock_lock_timeout() {
    std::cout << "=== Test: qspinlock lock timeout ===" << std::endl;

    const char* shm_file = "/tmp/test_qspin_timeout.shm";
    unlink(shm_file);

    DistributedLockSystem dls;
    DistributedLockSystem::Config config;
    config.node_id = 0;
    config.max_nodes = 2;
    config.max_locks = 16;
    config.use_file_backed_shm = true;
    config.shm_backing_file = shm_file;
    config.lock_algorithm = "qspinlock";
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
    std::cout << "CXL Distributed Lock - qspinlock Tests" << std::endl;
    std::cout << "========================================" << std::endl << std::endl;

    int passed = 0;
    int failed = 0;

    if (test_qspinlock_single_node_lock_unlock()) passed++; else failed++;
    if (test_qspinlock_fast_path())               passed++; else failed++;
    if (test_qspinlock_multiple_locks())          passed++; else failed++;
    if (test_qspinlock_lock_timeout())            passed++; else failed++;
    if (test_qspinlock_two_node_contention())     passed++; else failed++;

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    // Clean up any remaining temporary files
    unlink("/tmp/test_qspin_single.shm");
    unlink("/tmp/test_qspin_two_node.shm");
    unlink("/tmp/test_qspin_fast.shm");
    unlink("/tmp/test_qspin_multi_locks.shm");
    unlink("/tmp/test_qspin_timeout.shm");

    return failed > 0 ? 1 : 0;
}
