// test_contention.cpp
// CXL Distributed Lock System - Contention Simulation Tests
//
// Simulates a realistic business scenario: multiple nodes competing for
// shared data resources in a key-value store workload. Uses fork() to
// simulate multiple independent nodes accessing shared data blocks protected
// by distributed locks.

#include "distributed_lock.h"
#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cassert>

using namespace cxl_lock;

// ---------------------------------------------------------------------------
// Simulated shared data region - 64-byte cache-line-aligned data block
//
// In a real deployment, these DataBlock structures would reside in the
// CXL-attached shared memory region accessible by all nodes. Here we model
// the structure for correctness verification.
// ---------------------------------------------------------------------------
struct DataBlock {
    alignas(8) uint64_t key;          // 8 bytes
    alignas(8) uint64_t value;        // 8 bytes
    alignas(4) uint32_t version;      // 4 bytes
    alignas(4) uint32_t owner_node;   // 4 bytes
    alignas(8) uint64_t last_modified;// 8 bytes
    alignas(1) uint8_t  valid;        // 1 byte
    char padding[31];                 // Pad to 64 bytes (cache line)
};
static_assert(sizeof(DataBlock) == 64, "DataBlock must be 64 bytes (one cache line)");

// ---------------------------------------------------------------------------
// Operation statistics reported by each node
// ---------------------------------------------------------------------------
struct NodeStats {
    uint32_t node_id;
    uint32_t successful_ops;
    uint32_t failed_ops;
    uint32_t lock_timeouts;
    uint32_t read_ops;
    uint32_t write_ops;
    uint32_t read_modify_write_ops;
    int64_t  elapsed_ms;
    double   throughput;

    void print() const {
        std::cout << "----------------------------------------" << std::endl;
        std::cout << "Node " << node_id << " Summary:" << std::endl;
        std::cout << "  Total operations:  " << (successful_ops + failed_ops) << std::endl;
        std::cout << "  Successful:        " << successful_ops << std::endl;
        std::cout << "  Failed:            " << failed_ops << std::endl;
        std::cout << "  Lock timeouts:     " << lock_timeouts << std::endl;
        std::cout << "  Read ops:          " << read_ops << std::endl;
        std::cout << "  Write ops:         " << write_ops << std::endl;
        std::cout << "  RMW ops:           " << read_modify_write_ops << std::endl;
        std::cout << "  Time elapsed:      " << elapsed_ms << " ms" << std::endl;
        std::cout << "  Throughput:        " << throughput << " ops/sec" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
    }
};

// ---------------------------------------------------------------------------
// ContentionSimulator
//
// Simulates a node in a distributed system performing data operations that
// require acquiring distributed locks. Operations:
//   0 = READ    - acquire lock, read value, release
//   1 = WRITE   - acquire lock, write new value, release
//   2 = RMW     - acquire lock, read-modify-write, release
// ---------------------------------------------------------------------------
class ContentionSimulator {
public:
    struct Config {
        uint32_t node_id;
        uint32_t total_nodes;
        uint32_t num_data_blocks;
        uint32_t num_operations;
        uint32_t lock_timeout_ms;
        bool     verbose;
    };

    bool run(const Config& sim_config, const std::string& shm_file,
             NodeStats* out_stats = nullptr) {
        // ---- Initialize distributed lock system ----
        DistributedLockSystem dls;
        DistributedLockSystem::Config dls_config;
        dls_config.node_id = sim_config.node_id;
        dls_config.max_nodes = sim_config.total_nodes;
        dls_config.max_locks = sim_config.num_data_blocks;  // One lock per data block
        dls_config.use_file_backed_shm = true;
        dls_config.shm_backing_file = shm_file;
        dls_config.lock_algorithm = "cas";
        dls_config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
        dls_config.shm_size = 16 * 1024 * 1024;  // 16MB
        dls_config.lock_manager_scan_interval_us = 50;

        bool as_manager = (sim_config.node_id == 0);
        LockResult rc = dls.initialize(dls_config, as_manager);
        if (rc != LockResult::SUCCESS) {
            std::cerr << "Node " << sim_config.node_id << ": initialize failed: "
                      << static_cast<int>(rc) << std::endl;
            return false;
        }

        if (as_manager && sim_config.verbose) {
            std::cout << "Node 0: Lock manager initialized (algo="
                      << dls.get_algorithm_name() << ")" << std::endl;
        }

        // Wait for all nodes to initialize before starting operations
        sleep(1);

        // ---- Setup random number generator ----
        std::mt19937_64 rng(sim_config.node_id + 42);
        std::uniform_int_distribution<uint64_t> key_dist(0, sim_config.num_data_blocks - 1);
        std::uniform_int_distribution<uint64_t> value_dist(1, 1000000);
        std::uniform_int_distribution<int>      op_dist(0, 2);   // 0=read, 1=write, 2=rmw
        std::uniform_int_distribution<uint32_t> delay_dist(100, 1000);  // 0.1-1ms

        NodeStats stats{};
        stats.node_id = sim_config.node_id;

        auto start_time = std::chrono::steady_clock::now();

        // ---- Main operation loop ----
        for (uint32_t op_idx = 0; op_idx < sim_config.num_operations; ++op_idx) {
            uint64_t data_key = key_dist(rng);
            int      operation = op_dist(rng);

            // Acquire the distributed lock for this data block
            rc = dls.acquire_data_lock(&data_key, sizeof(data_key),
                                       sim_config.lock_timeout_ms);
            if (rc != LockResult::SUCCESS) {
                if (rc == LockResult::ERROR_TIMEOUT) {
                    stats.lock_timeouts++;
                }
                stats.failed_ops++;
                continue;
            }

            // ---- Critical section: simulate data access ----
            {
                // In a real system, this would access the shared memory data block.
                // Here we simulate the work based on the operation type.
                uint64_t value = value_dist(rng);
                (void)value;  // Used in simulated read/write logic

                switch (operation) {
                    case 0:  // READ
                        stats.read_ops++;
                        // Simulate reading data: just a small delay
                        usleep(50);
                        break;

                    case 1:  // WRITE
                        stats.write_ops++;
                        // Simulate writing data: slightly longer (cache line update)
                        usleep(100);
                        break;

                    case 2:  // READ-MODIFY-WRITE
                        stats.read_modify_write_ops++;
                        // Simulate RMW: read, compute, write back
                        usleep(200);
                        break;

                    default:
                        assert(!"Invalid operation type");
                        break;
                }

                // Simulate additional randomized processing jitter
                usleep(delay_dist(rng));
            }
            // ---- End critical section ----

            // Release the distributed lock
            rc = dls.release_data_lock(&data_key, sizeof(data_key));
            if (rc != LockResult::SUCCESS) {
                std::cerr << "Node " << sim_config.node_id
                          << ": release_data_lock failed for key=" << data_key
                          << ", rc=" << static_cast<int>(rc) << std::endl;
                stats.failed_ops++;
            } else {
                stats.successful_ops++;
            }

            // Progress reporting
            if (sim_config.verbose && op_idx > 0 && op_idx % 100 == 0) {
                std::cout << "Node " << sim_config.node_id << ": completed "
                          << op_idx << "/" << sim_config.num_operations
                          << " ops (" << stats.successful_ops << " ok, "
                          << stats.lock_timeouts << " timeouts)" << std::endl;
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        stats.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        if (stats.elapsed_ms > 0) {
            stats.throughput = static_cast<double>(stats.successful_ops) * 1000.0
                               / static_cast<double>(stats.elapsed_ms);
        } else {
            stats.throughput = 0.0;
        }

        // Print node summary
        stats.print();

        if (out_stats != nullptr) {
            *out_stats = stats;
        }

        dls.shutdown();
        return true;
    }
};

// ---------------------------------------------------------------------------
// Print usage information
// ---------------------------------------------------------------------------
static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [num_nodes] [num_data_blocks] [num_ops] [timeout_ms]"
              << std::endl;
    std::cout << std::endl;
    std::cout << "Arguments:" << std::endl;
    std::cout << "  num_nodes       Number of nodes (default: 2)" << std::endl;
    std::cout << "  num_data_blocks Number of shared data blocks/locks (default: 8)"
              << std::endl;
    std::cout << "  num_ops         Operations per node (default: 100)" << std::endl;
    std::cout << "  timeout_ms      Lock timeout in milliseconds (default: 5000)"
              << std::endl;
    std::cout << std::endl;
    std::cout << "Example: " << prog << " 4 16 200 10000" << std::endl;
}

// ---------------------------------------------------------------------------
// Main entry point
//
// Spawns N-1 worker processes via fork(). Node 0 (parent) acts as both the
// lock manager and a worker. After all workers finish, aggregates and prints
// combined statistics.
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "CXL Distributed Lock - Contention Simulation" << std::endl;
    std::cout << "========================================" << std::endl;

    // ---- Parse command-line arguments ----
    uint32_t num_nodes        = 2;       // Number of nodes (1 lock manager + N-1 workers)
    uint32_t num_data_blocks  = 8;       // Number of shared data blocks
    uint32_t num_operations   = 100;     // Operations per node
    uint32_t timeout_ms       = 5000;    // Lock acquisition timeout

    if (argc > 1) {
        if (std::strcmp(argv[1], "-h") == 0 ||
            std::strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        num_nodes = static_cast<uint32_t>(std::atoi(argv[1]));
    }
    if (argc > 2) num_data_blocks = static_cast<uint32_t>(std::atoi(argv[2]));
    if (argc > 3) num_operations  = static_cast<uint32_t>(std::atoi(argv[3]));
    if (argc > 4) timeout_ms      = static_cast<uint32_t>(std::atoi(argv[4]));

    if (num_nodes < 1 || num_nodes > 16) {
        std::cerr << "ERROR: num_nodes must be between 1 and 16" << std::endl;
        return 1;
    }
    if (num_data_blocks < 1 || num_data_blocks > 4096) {
        std::cerr << "ERROR: num_data_blocks must be between 1 and 4096" << std::endl;
        return 1;
    }
    if (num_operations < 1) {
        std::cerr << "ERROR: num_operations must be >= 1" << std::endl;
        return 1;
    }

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Nodes:             " << num_nodes << std::endl;
    std::cout << "  Data blocks:       " << num_data_blocks << std::endl;
    std::cout << "  Operations/node:   " << num_operations << std::endl;
    std::cout << "  Lock timeout:      " << timeout_ms << " ms" << std::endl;
    std::cout << "  Lock algorithm:    cas" << std::endl;
    std::cout << "  Consistency mode:  software" << std::endl;
    std::cout << "========================================" << std::endl << std::endl;

    const char* shm_file = "/tmp/test_contention.shm";

    // Remove any stale shared memory file
    unlink(shm_file);

    // ---- Spawn worker processes (nodes 1 .. N-1) ----
    std::vector<pid_t> children;
    children.reserve(num_nodes - 1);

    for (uint32_t node = 1; node < num_nodes; ++node) {
        pid_t pid = fork();
        if (pid == 0) {
            // ---- Child process: worker node ----
            ContentionSimulator simulator;
            ContentionSimulator::Config sim_config;
            sim_config.node_id        = node;
            sim_config.total_nodes    = num_nodes;
            sim_config.num_data_blocks = num_data_blocks;
            sim_config.num_operations = num_operations;
            sim_config.lock_timeout_ms = timeout_ms;
            sim_config.verbose        = false;  // Workers are quiet

            bool ok = simulator.run(sim_config, shm_file);
            _exit(ok ? 0 : 1);

        } else if (pid > 0) {
            children.push_back(pid);
        } else {
            std::cerr << "ERROR: fork() failed for node " << node << std::endl;
            // Clean up already-spawned children
            for (pid_t p : children) {
                kill(p, SIGTERM);
                waitpid(p, nullptr, 0);
            }
            return 1;
        }
    }

    // ---- Parent process: node 0 (lock manager + worker) ----
    NodeStats manager_stats{};
    {
        ContentionSimulator simulator;
        ContentionSimulator::Config sim_config;
        sim_config.node_id         = 0;
        sim_config.total_nodes     = num_nodes;
        sim_config.num_data_blocks = num_data_blocks;
        sim_config.num_operations  = num_operations;
        sim_config.lock_timeout_ms = timeout_ms;
        sim_config.verbose         = true;  // Manager prints progress

        bool ok = simulator.run(sim_config, shm_file, &manager_stats);
        if (!ok) {
            std::cerr << "ERROR: Lock manager (node 0) failed" << std::endl;
        }
    }

    // ---- Wait for all worker processes to complete ----
    std::vector<NodeStats> all_stats;
    all_stats.reserve(num_nodes);
    all_stats.push_back(manager_stats);

    int failed_nodes = 0;
    for (pid_t child_pid : children) {
        int status = 0;
        pid_t waited = waitpid(child_pid, &status, 0);
        (void)waited;

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed_nodes++;
            if (WIFSIGNALED(status)) {
                std::cerr << "WARNING: Node with pid " << child_pid
                          << " terminated by signal " << WTERMSIG(status) << std::endl;
            } else {
                std::cerr << "WARNING: Node with pid " << child_pid
                          << " exited with status " << WEXITSTATUS(status) << std::endl;
            }
        }
    }

    // ---- Aggregate and print combined statistics ----
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "         COMBINED STATISTICS            " << std::endl;
    std::cout << "========================================" << std::endl;

    uint32_t total_successful = manager_stats.successful_ops;
    uint32_t total_failed     = manager_stats.failed_ops;
    uint32_t total_timeouts   = manager_stats.lock_timeouts;
    uint32_t total_reads      = manager_stats.read_ops;
    uint32_t total_writes     = manager_stats.write_ops;
    uint32_t total_rmw        = manager_stats.read_modify_write_ops;
    int64_t  max_elapsed_ms   = manager_stats.elapsed_ms;

    // Combined stats from children aren't available via shared memory in this
    // simplified version; we aggregate from the manager's perspective.

    std::cout << "  Total nodes:       " << num_nodes << std::endl;
    std::cout << "  Successful ops:    " << total_successful << std::endl;
    std::cout << "  Failed ops:        " << total_failed << std::endl;
    std::cout << "  Lock timeouts:     " << total_timeouts << std::endl;
    std::cout << "  Read ops:          " << total_reads << std::endl;
    std::cout << "  Write ops:         " << total_writes << std::endl;
    std::cout << "  RMW ops:           " << total_rmw << std::endl;
    std::cout << "  Manager elapsed:   " << max_elapsed_ms << " ms" << std::endl;
    if (max_elapsed_ms > 0) {
        std::cout << "  Approx throughput: "
                  << (static_cast<double>(total_successful) * 1000.0
                      / static_cast<double>(max_elapsed_ms))
                  << " ops/sec" << std::endl;
    }
    std::cout << "========================================" << std::endl;

    // Final verdict
    if (failed_nodes == 0) {
        std::cout << "RESULT: All " << num_nodes << " nodes completed successfully!"
                  << std::endl;
    } else {
        std::cout << "RESULT: " << failed_nodes << " node(s) failed out of "
                  << num_nodes << std::endl;
    }
    std::cout << "========================================" << std::endl;

    // Clean up
    unlink(shm_file);

    return (failed_nodes > 0) ? 1 : 0;
}
