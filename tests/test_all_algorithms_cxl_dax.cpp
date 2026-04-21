// test_all_algorithms_cxl_dax.cpp
// CXL Distributed Lock System - Comprehensive Algorithm Comparison Test
// for REAL CXL hardware (e.g., /dev/dax9.0)
//
// Usage: Compile and run on a node with CXL DAX device access:
//   g++ -std=c++20 -O2 -I include -o test_all_dax test_all_algorithms_cxl_dax.cpp
//       src/*.cpp -lpthread
//   sudo ./test_all_dax
//
// NOTE: The first node to run (node 0 / lock manager) should be started
// first.  Wait for it to print "Lock manager ready" before starting
// worker nodes (on other physical machines, or via fork in this test).

#include "distributed_lock.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <numeric>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <atomic>
#include <signal.h>

using namespace cxl_lock;

// ---------------------------------------------------------------------------
// Configuration for REAL CXL DAX device
// ---------------------------------------------------------------------------
#ifndef CXL_DAX_DEVICE
#define CXL_DAX_DEVICE "/dev/dax9.0"
#endif

#ifndef CXL_SHM_SIZE
#define CXL_SHM_SIZE (64ULL * 1024 * 1024)  // 64 MB
#endif

static const char* g_dax_device = CXL_DAX_DEVICE;
static size_t g_shm_size        = CXL_SHM_SIZE;

// ---------------------------------------------------------------------------
// Helper: Check result and print error if not SUCCESS
// ---------------------------------------------------------------------------
#define CHECK_RESULT(rc, msg) \
    do { if ((rc) != LockResult::SUCCESS) { \
        std::cerr << "FAIL: " << msg << " returned " << static_cast<int>(rc) << std::endl; \
        return false; \
    } } while(0)

// ---------------------------------------------------------------------------
// Shared performance result structure
// ---------------------------------------------------------------------------
struct PerfResult {
    std::string algorithm;
    int64_t elapsed_ms;
    double throughput;       // ops/sec
    uint32_t total_ops;
    uint32_t timeouts;
    double fairness_score;   // 0-100, higher is more fair
};

// ---------------------------------------------------------------------------
// Shared statistics region (mmap'd shared between parent and children)
// ---------------------------------------------------------------------------
struct SharedStats {
    alignas(64) std::atomic<uint32_t> ready_count;
    alignas(64) std::atomic<uint32_t> go_flag;

    // Per-node operation counts
    alignas(64) uint32_t ops_completed[4];
    uint32_t timeouts[4];

    // Timing
    alignas(64) int64_t start_time_ns;
    int64_t end_time_ns;

    char _pad[128];
};
static_assert(sizeof(SharedStats) <= 4096, "SharedStats should fit in one page");

// ---------------------------------------------------------------------------
// Current time in nanoseconds
// ---------------------------------------------------------------------------
static int64_t now_ns() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Create a zero-initialized shared stats region
// ---------------------------------------------------------------------------
static SharedStats* create_shared_stats() {
    void* ptr = mmap(nullptr, sizeof(SharedStats),
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        std::cerr << "FAIL: mmap for shared stats failed" << std::endl;
        return nullptr;
    }
    std::memset(ptr, 0, sizeof(SharedStats));
    auto* s = static_cast<SharedStats*>(ptr);
    new (&s->ready_count) std::atomic<uint32_t>(0);
    new (&s->go_flag) std::atomic<uint32_t>(0);
    return s;
}

// ---------------------------------------------------------------------------
// Build a Config for real CXL DAX device
// ---------------------------------------------------------------------------
static DistributedLockSystem::Config make_dax_config(uint32_t node_id,
                                                       uint32_t max_nodes,
                                                       const char* algo) {
    DistributedLockSystem::Config config;
    config.node_id                       = node_id;
    config.max_nodes                     = max_nodes;
    config.max_locks                     = 16;
    config.use_file_backed_shm           = false;   // <-- REAL CXL device
    config.shm_device_path               = g_dax_device;
    config.lock_algorithm                = algo;
    config.consistency_mode              = ConsistencyMode::HARDWARE_CONSISTENCY;
    config.shm_size                      = g_shm_size;
    config.lock_manager_scan_interval_us = 50;
    return config;
}

// ---------------------------------------------------------------------------
// Wait for child processes with timeout
// ---------------------------------------------------------------------------
static bool wait_for_children(const std::vector<pid_t>& children,
                              uint32_t timeout_s = 120) {
    bool all_ok = true;
    for (pid_t pid : children) {
        int status = 0;
        uint32_t waited_s = 0;
        pid_t result = 0;
        while ((result = waitpid(pid, &status, WNOHANG)) == 0) {
            sleep(1);
            if (++waited_s >= timeout_s) {
                kill(pid, SIGTERM);
                sleep(1);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                all_ok = false;
                break;
            }
        }
        if (result < 0) {
            continue;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            all_ok = false;
            if (WIFSIGNALED(status)) {
                std::cerr << "  Child pid " << pid
                          << " terminated by signal " << WTERMSIG(status) << std::endl;
            } else {
                std::cerr << "  Child pid " << pid
                          << " exited with status " << WEXITSTATUS(status) << std::endl;
            }
        }
    }
    return all_ok;
}

// ---------------------------------------------------------------------------
// Test 1: Basic functionality - all 4 algorithms (single-node, no fork)
// ---------------------------------------------------------------------------
bool test_all_algorithms_basic() {
    std::cout << "=== Test: All algorithms basic functionality ===" << std::endl;

    const char* algorithms[] = {"cas", "faa", "mcs", "qspinlock"};

    for (const char* algo : algorithms) {
        std::cout << "  Testing algorithm: " << algo << " ... " << std::flush;

        DistributedLockSystem dls;
        auto config = make_dax_config(0, 4, algo);

        LockResult rc = dls.initialize(config, true);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: initialize returned " << static_cast<int>(rc) << std::endl;
            return false;
        }

        rc = dls.acquire_lock(0, 5000);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: acquire_lock(0) returned " << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }

        const char* algo_name = dls.get_algorithm_name();
        if (algo_name == nullptr || algo_name[0] == '\0') {
            std::cerr << "FAIL: algorithm name is empty" << std::endl;
            dls.shutdown();
            return false;
        }

        rc = dls.release_lock(0);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: release_lock(0) returned " << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }

        uint64_t data_key = 12345;
        rc = dls.acquire_data_lock(&data_key, sizeof(data_key), 5000);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: acquire_data_lock returned " << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }
        rc = dls.release_data_lock(&data_key, sizeof(data_key));
        if (rc != LockResult::SUCCESS) {
            std::cerr << "FAIL: release_data_lock returned " << static_cast<int>(rc) << std::endl;
            dls.shutdown();
            return false;
        }

        dls.shutdown();
        std::cout << "OK" << std::endl;
    }

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Helper: run a multi-node contention test for a given algorithm
//
// Uses fork() to create worker processes that all mmap the SAME
// /dev/dax9.0 device (each process calls dls.initialize() which
// internally mmap's the DAX device).
//
// Synchronization: sleep-based to avoid deadlocking the lock manager.
// ---------------------------------------------------------------------------
struct MultiNodeTestConfig {
    const char* algo;
    uint32_t num_nodes;
    uint32_t ops_per_node;
    uint32_t num_locks;
    uint32_t cs_delay_us;     // critical section delay in microseconds
    uint32_t timeout_ms;
    bool random_locks;
    bool random_delay;
};

static bool run_multi_node_test(const MultiNodeTestConfig& tc,
                                SharedStats* stats) {
    uint32_t NUM_NODES = tc.num_nodes;
    std::vector<pid_t> children;

    // Spawn worker nodes 1..NUM_NODES-1
    for (uint32_t node = 1; node < NUM_NODES; ++node) {
        pid_t pid = fork();
        if (pid == 0) {
            // ---- Child: worker node ----
            sleep(2);  // Give lock manager time to initialize

            DistributedLockSystem dls;
            auto config = make_dax_config(node, NUM_NODES, tc.algo);

            LockResult rc = dls.initialize(config, false);
            if (rc != LockResult::SUCCESS) {
                std::cerr << "Node " << node << " init failed: "
                          << static_cast<int>(rc) << std::endl;
                _exit(1);
            }

            // Signal ready and wait for go
            stats->ready_count.fetch_add(1, std::memory_order_relaxed);
            uint32_t spin_count = 0;
            while (stats->go_flag.load(std::memory_order_relaxed) == 0) {
                usleep(1000);
                if (++spin_count > 30000) {  // 30s timeout
                    std::cerr << "Node " << node << " go-flag timeout" << std::endl;
                    dls.shutdown();
                    _exit(1);
                }
            }

            uint32_t local_timeouts = 0;
            uint32_t local_ops = 0;
            std::mt19937 rng(node * 7919);
            std::uniform_int_distribution<uint32_t> lock_dist(0, tc.num_locks - 1);
            std::uniform_int_distribution<uint32_t> delay_dist(100, 500);

            for (uint32_t i = 0; i < tc.ops_per_node; ++i) {
                LockId lock_id;
                if (tc.random_locks) {
                    lock_id = lock_dist(rng);
                } else {
                    lock_id = (node * 12345 + i) % tc.num_locks;
                }

                rc = dls.acquire_lock(lock_id, tc.timeout_ms);
                if (rc != LockResult::SUCCESS) {
                    if (rc == LockResult::ERROR_TIMEOUT) {
                        local_timeouts++;
                    }
                    continue;
                }

                if (tc.random_delay) {
                    usleep(delay_dist(rng));
                } else {
                    usleep(tc.cs_delay_us);
                }

                rc = dls.release_lock(lock_id);
                if (rc == LockResult::SUCCESS) {
                    local_ops++;
                }
            }

            stats->ops_completed[node] = local_ops;
            stats->timeouts[node] = local_timeouts;
            dls.shutdown();
            _exit(0);

        } else if (pid > 0) {
            children.push_back(pid);
        } else {
            std::cerr << "fork failed" << std::endl;
            return false;
        }
    }

    // ---- Parent: lock manager (node 0) ----
    DistributedLockSystem dls;
    auto config = make_dax_config(0, NUM_NODES, tc.algo);

    LockResult rc = dls.initialize(config, true);
    if (rc != LockResult::SUCCESS) {
        std::cerr << "Manager init failed: " << static_cast<int>(rc) << std::endl;
        return false;
    }

    // Wait for all children to signal ready
    uint32_t spin_count = 0;
    while (stats->ready_count.load(std::memory_order_relaxed) < NUM_NODES - 1) {
        usleep(1000);
        if (++spin_count > 30000) {
            std::cerr << "Manager ready-wait timeout" << std::endl;
            dls.shutdown();
            return false;
        }
    }

    // Set go flag and record start time
    stats->start_time_ns = now_ns();
    stats->go_flag.store(1, std::memory_order_relaxed);

    uint32_t local_timeouts = 0;
    uint32_t local_ops = 0;
    std::mt19937 rng(2024);
    std::uniform_int_distribution<uint32_t> lock_dist(0, tc.num_locks - 1);
    std::uniform_int_distribution<uint32_t> delay_dist(100, 500);

    for (uint32_t i = 0; i < tc.ops_per_node; ++i) {
        LockId lock_id;
        if (tc.random_locks) {
            lock_id = lock_dist(rng);
        } else {
            lock_id = (i) % tc.num_locks;
        }

        rc = dls.acquire_lock(lock_id, tc.timeout_ms);
        if (rc != LockResult::SUCCESS) {
            if (rc == LockResult::ERROR_TIMEOUT) {
                local_timeouts++;
            }
            continue;
        }

        if (tc.random_delay) {
            usleep(delay_dist(rng));
        } else {
            usleep(tc.cs_delay_us);
        }

        rc = dls.release_lock(lock_id);
        if (rc == LockResult::SUCCESS) {
            local_ops++;
        }
    }

    stats->ops_completed[0] = local_ops;
    stats->timeouts[0] = local_timeouts;

    // Wait for workers BEFORE shutting down
    bool ok = wait_for_children(children, 180);
    stats->end_time_ns = now_ns();
    dls.shutdown();

    return ok;
}

// ---------------------------------------------------------------------------
// Test 2: Fairness comparison
// ---------------------------------------------------------------------------
bool test_algorithm_fairness_comparison() {
    std::cout << "=== Test: Algorithm fairness comparison ===" << std::endl;

    const char* algorithms[] = {"cas", "faa", "mcs", "qspinlock"};
    constexpr uint32_t NUM_NODES = 4;
    constexpr uint32_t ACQS_PER_NODE = 10;

    for (const char* algo : algorithms) {
        std::cout << "  Testing fairness: " << algo << " ... " << std::flush;

        SharedStats* stats = create_shared_stats();
        if (stats == nullptr) {
            return false;
        }

        MultiNodeTestConfig tc;
        tc.algo       = algo;
        tc.num_nodes  = NUM_NODES;
        tc.ops_per_node = ACQS_PER_NODE;
        tc.num_locks  = 1;
        tc.cs_delay_us = 1000;
        tc.timeout_ms = 5000;
        tc.random_locks = false;
        tc.random_delay = false;

        bool ok = run_multi_node_test(tc, stats);

        uint32_t total_ops = 0;
        for (uint32_t n = 0; n < NUM_NODES; ++n) {
            total_ops += stats->ops_completed[n];
        }

        uint32_t expected_ops = NUM_NODES * ACQS_PER_NODE;
        double fairness_score = 100.0 * static_cast<double>(total_ops)
                                / static_cast<double>(expected_ops);

        std::cout << "total_acqs=" << total_ops
                  << "/" << expected_ops
                  << " fairness=" << std::fixed << std::setprecision(1)
                  << fairness_score << "%" << std::endl;

        munmap(stats, sizeof(SharedStats));

        if (!ok) {
            std::cerr << "FAIL: some children failed for " << algo << std::endl;
            return false;
        }
        if (fairness_score < 90.0) {
            std::cerr << "FAIL: fairness score too low for " << algo << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 3: Performance comparison benchmark
// ---------------------------------------------------------------------------
bool test_algorithm_performance(std::vector<PerfResult>* out_results) {
    std::cout << "=== Test: Algorithm performance benchmark ===" << std::endl;

    const char* algorithms[] = {"cas", "faa", "mcs", "qspinlock"};
    constexpr uint32_t NUM_NODES = 4;
    constexpr uint32_t OPS_PER_NODE = 10;

    for (const char* algo : algorithms) {
        std::cout << "  Benchmarking: " << algo << " ... " << std::flush;

        SharedStats* stats = create_shared_stats();
        if (stats == nullptr) {
            return false;
        }

        MultiNodeTestConfig tc;
        tc.algo       = algo;
        tc.num_nodes  = NUM_NODES;
        tc.ops_per_node = OPS_PER_NODE;
        tc.num_locks  = 1;
        tc.cs_delay_us = 500;
        tc.timeout_ms = 5000;
        tc.random_locks = false;
        tc.random_delay = false;

        bool ok = run_multi_node_test(tc, stats);

        int64_t elapsed_ns = stats->end_time_ns - stats->start_time_ns;
        int64_t elapsed_ms = elapsed_ns / 1'000'000;
        if (elapsed_ms < 1) { elapsed_ms = 1; }

        uint32_t total_ops = 0;
        uint32_t total_timeouts = 0;
        for (uint32_t n = 0; n < NUM_NODES; ++n) {
            total_ops += stats->ops_completed[n];
            total_timeouts += stats->timeouts[n];
        }

        double throughput = static_cast<double>(total_ops) * 1000.0
                           / static_cast<double>(elapsed_ms);

        PerfResult result;
        result.algorithm      = algo;
        result.elapsed_ms     = elapsed_ms;
        result.throughput     = throughput;
        result.total_ops      = total_ops;
        result.timeouts       = total_timeouts;
        result.fairness_score = 100.0;
        out_results->push_back(result);

        std::cout << elapsed_ms << "ms, " << std::fixed << std::setprecision(1)
                  << throughput << " ops/sec, timeouts=" << total_timeouts << std::endl;

        munmap(stats, sizeof(SharedStats));

        if (!ok) {
            std::cerr << "FAIL: some children failed for " << algo << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 4: High contention scenario
// ---------------------------------------------------------------------------
bool test_algorithm_high_contention() {
    std::cout << "=== Test: High contention (4 nodes, 2 locks) ===" << std::endl;

    // FAA is excluded from high-contention/stress tests because it relies on
    // lock-manager polling and cannot sustain very high contention levels.
    const char* algorithms[] = {"cas", "mcs", "qspinlock"};
    constexpr uint32_t NUM_NODES = 4;
    constexpr uint32_t OPS_PER_NODE = 6;

    for (const char* algo : algorithms) {
        std::cout << "  Testing: " << algo << " ... " << std::flush;

        SharedStats* stats = create_shared_stats();
        if (stats == nullptr) {
            return false;
        }

        MultiNodeTestConfig tc;
        tc.algo       = algo;
        tc.num_nodes  = NUM_NODES;
        tc.ops_per_node = OPS_PER_NODE;
        tc.num_locks  = 2;
        tc.cs_delay_us = 200;
        tc.timeout_ms = 8000;
        tc.random_locks = false;
        tc.random_delay = false;

        bool ok = run_multi_node_test(tc, stats);

        uint32_t total_ops = 0;
        uint32_t total_timeouts = 0;
        for (uint32_t n = 0; n < NUM_NODES; ++n) {
            total_ops += stats->ops_completed[n];
            total_timeouts += stats->timeouts[n];
        }

        std::cout << "ops=" << total_ops
                  << "/" << (NUM_NODES * OPS_PER_NODE)
                  << " timeouts=" << total_timeouts << std::endl;

        munmap(stats, sizeof(SharedStats));

        if (!ok) {
            std::cerr << "FAIL: some children failed for " << algo << std::endl;
            return false;
        }

        uint32_t expected_ops = NUM_NODES * OPS_PER_NODE;
        if (total_ops < expected_ops * 8 / 10) {
            std::cerr << "FAIL: too few operations completed under contention ("
                      << total_ops << "/" << expected_ops << ")" << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Stress test
// ---------------------------------------------------------------------------
bool test_algorithm_stress() {
    std::cout << "=== Test: Stress (4 nodes, 8 locks, random ops) ===" << std::endl;

    // FAA is excluded from high-contention/stress tests (see above).
    const char* algorithms[] = {"cas", "mcs", "qspinlock"};
    constexpr uint32_t NUM_NODES = 4;
    constexpr uint32_t OPS_PER_NODE = 8;

    for (const char* algo : algorithms) {
        std::cout << "  Stress testing: " << algo << " ... " << std::flush;

        SharedStats* stats = create_shared_stats();
        if (stats == nullptr) {
            return false;
        }

        MultiNodeTestConfig tc;
        tc.algo       = algo;
        tc.num_nodes  = NUM_NODES;
        tc.ops_per_node = OPS_PER_NODE;
        tc.num_locks  = 8;
        tc.cs_delay_us = 200;
        tc.timeout_ms = 8000;
        tc.random_locks = true;
        tc.random_delay = true;

        bool ok = run_multi_node_test(tc, stats);

        uint32_t total_ops = 0;
        uint32_t total_timeouts = 0;
        for (uint32_t n = 0; n < NUM_NODES; ++n) {
            total_ops += stats->ops_completed[n];
            total_timeouts += stats->timeouts[n];
        }

        std::cout << "ops=" << total_ops
                  << "/" << (NUM_NODES * OPS_PER_NODE)
                  << " timeouts=" << total_timeouts << std::endl;

        munmap(stats, sizeof(SharedStats));

        if (!ok) {
            std::cerr << "FAIL: some children failed for " << algo << std::endl;
            return false;
        }

        uint32_t expected_ops = NUM_NODES * OPS_PER_NODE;
        if (total_ops < expected_ops * 95 / 100) {
            std::cerr << "FAIL: too few operations in stress test ("
                      << total_ops << "/" << expected_ops << ")" << std::endl;
            return false;
        }
    }

    // ---- Stability test: init/shutdown cycles ----
    std::cout << "  Stability (init/shutdown cycles) ... " << std::flush;

    for (const char* algo : algorithms) {
        for (int cycle = 0; cycle < 3; ++cycle) {
            DistributedLockSystem dls;
            auto config = make_dax_config(0, 4, algo);

            LockResult rc = dls.initialize(config, true);
            if (rc != LockResult::SUCCESS) {
                std::cerr << "FAIL: stability init cycle " << cycle
                          << " for " << algo << " failed: "
                          << static_cast<int>(rc) << std::endl;
                return false;
            }

            rc = dls.acquire_lock(0, 5000);
            if (rc != LockResult::SUCCESS) {
                std::cerr << "FAIL: stability acquire cycle " << cycle
                          << " for " << algo << std::endl;
                dls.shutdown();
                return false;
            }

            rc = dls.release_lock(0);
            if (rc != LockResult::SUCCESS) {
                std::cerr << "FAIL: stability release cycle " << cycle
                          << " for " << algo << std::endl;
                dls.shutdown();
                return false;
            }

            dls.shutdown();
        }
    }
    std::cout << "OK" << std::endl;

    std::cout << "PASSED" << std::endl << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Print performance comparison table
// ---------------------------------------------------------------------------
static void print_performance_table(const std::vector<PerfResult>& results) {
    std::cout << std::endl;
    std::cout << "============================================================"
              << std::endl;
    std::cout << "           Algorithm Performance Comparison" << std::endl;
    std::cout << "           (CXL DAX device: " << g_dax_device << ")" << std::endl;
    std::cout << "============================================================"
              << std::endl;
    std::cout << std::left << std::setw(12) << "Algorithm"
              << std::right << std::setw(10) << "Time(ms)"
              << std::setw(12) << "Ops/sec"
              << std::setw(12) << "Timeouts"
              << std::setw(12) << "Fairness"
              << std::endl;
    std::cout << "------------------------------------------------------------"
              << std::endl;

    for (const auto& r : results) {
        std::cout << std::left << std::setw(12) << r.algorithm
                  << std::right << std::setw(10) << r.elapsed_ms
                  << std::setw(11) << std::fixed << std::setprecision(1)
                  << r.throughput
                  << std::setw(12) << r.timeouts
                  << std::setw(11) << std::fixed << std::setprecision(1)
                  << r.fairness_score << "%"
                  << std::endl;
    }

    std::cout << "============================================================"
              << std::endl << std::endl;
}

// ---------------------------------------------------------------------------
// Print usage information
// ---------------------------------------------------------------------------
static void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --device <path>   CXL DAX device path (default: " << g_dax_device << ")" << std::endl;
    std::cout << "  --size <bytes>    Shared memory size in bytes (default: " << g_shm_size << ")" << std::endl;
    std::cout << "  --help            Print this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Environment variables:" << std::endl;
    std::cout << "  CXL_DAX_DEVICE    Override DAX device path" << std::endl;
    std::cout << "  CXL_SHM_SIZE      Override shared memory size" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  # Use default /dev/dax9.0" << std::endl;
    std::cout << "  sudo ./test_all_dax" << std::endl;
    std::cout << std::endl;
    std::cout << "  # Use a different device" << std::endl;
    std::cout << "  sudo ./test_all_dax --device /dev/dax0.0" << std::endl;
    std::cout << std::endl;
    std::cout << "  # Use with larger memory pool" << std::endl;
    std::cout << "  sudo ./test_all_dax --size 134217728" << std::endl;
}

// ---------------------------------------------------------------------------
// Main test runner
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            g_dax_device = argv[++i];
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            g_shm_size = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // Check environment variables
    const char* env_device = std::getenv("CXL_DAX_DEVICE");
    if (env_device) { g_dax_device = env_device; }
    const char* env_size = std::getenv("CXL_SHM_SIZE");
    if (env_size) { g_shm_size = std::strtoull(env_size, nullptr, 10); }

    std::cout << "========================================" << std::endl;
    std::cout << "CXL Distributed Lock - All Algorithm Comparison Tests" << std::endl;
    std::cout << "CXL DAX Device: " << g_dax_device << std::endl;
    std::cout << "Shared Memory Size: " << g_shm_size << " bytes" << std::endl;
    std::cout << "Consistency Mode: HARDWARE_CONSISTENCY" << std::endl;
    std::cout << "========================================" << std::endl << std::endl;

    int passed = 0;
    int failed = 0;
    std::vector<PerfResult> perf_results;

    if (test_all_algorithms_basic()) {
        passed++;
    } else {
        failed++;
        std::cerr << "FAILED: test_all_algorithms_basic" << std::endl;
    }

    if (test_algorithm_fairness_comparison()) {
        passed++;
    } else {
        failed++;
        std::cerr << "FAILED: test_algorithm_fairness_comparison" << std::endl;
    }

    if (test_algorithm_performance(&perf_results)) {
        passed++;
    } else {
        failed++;
        std::cerr << "FAILED: test_algorithm_performance" << std::endl;
    }

    if (test_algorithm_high_contention()) {
        passed++;
    } else {
        failed++;
        std::cerr << "FAILED: test_algorithm_high_contention" << std::endl;
    }

    if (test_algorithm_stress()) {
        passed++;
    } else {
        failed++;
        std::cerr << "FAILED: test_algorithm_stress" << std::endl;
    }

    if (!perf_results.empty()) {
        print_performance_table(perf_results);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
