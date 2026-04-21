# CXL Distributed Lock - Change Log

## Summary

This release adds two new global lock algorithms (**MCS lock** and **qspinlock**) to the CXL Distributed Lock system, fixes two bugs in the existing FAA algorithm, and introduces comprehensive test coverage for all algorithms.

---

## New Files

### 1. MCS Global Lock Algorithm

| File | Description |
|------|-------------|
| `include/mcs_global_lock.h` | Header: `McsLockEntry`, `McsNode` data structures, `McsGlobalLockAlgorithm` class declaration |
| `src/mcs_global_lock.cpp` | Implementation: MCS queue-based FIFO lock algorithm |

**Design Overview:**
- **Shared memory layout:** `MAX_LOCK_COUNT` lock entries (64B each, cache-line aligned) followed by `node_count` MCS nodes (64B each).
- **Algorithm:** Each lock maintains a FIFO queue via a `tail_node` pointer. Each node has `next_node`, `locked` (0=free, 1=waiting, 2=locked), and `waiting_lock` fields.
- **Acquire:** CAS the tail to self; if queue was empty, lock manager grants. Otherwise, spin on `node->locked` until predecessor releases.
- **Release:** If no successor, CAS tail to `INVALID`. Otherwise, pass lock by setting `succ->locked = 2`.
- **Registration:** Registered as `"mcs"` via `REGISTER_LOCK_ALGORITHM`.

### 2. qspinlock Global Lock Algorithm

| File | Description |
|------|-------------|
| `include/qspinlock_global_lock.h` | Header: `QspinlockEntry`, `QspinNode` data structures, lock-word encoding macros, `QspinlockGlobalLockAlgorithm` class declaration |
| `src/qspinlock_global_lock.cpp` | Implementation: Linux-style hybrid queued spinlock |

**Design Overview:**
- **lock_word encoding:** Bit 0 = `locked`, bits 2-17 = `tail_idx` (node_id+1, 0=empty).
- **Fast path:** When `lock_word == 0`, CAS to `(my_tail << 2) | 1`. The acquirer becomes both holder and MCS queue head/tail.
- **Slow path:** Atomically link into MCS queue via CAS on `tail_idx`. Spin on `node->locked` until granted.
- **Release:** Pass lock to MCS successor by updating `lock_word` to successor's tail + locked bit, then set `succ->locked = 2`. If no successor, clear `lock_word` to 0.
- **Registration:** Registered as `"qspinlock"` via `REGISTER_LOCK_ALGORITHM`.

### 3. New Test Files

| File | Description |
|------|-------------|
| `tests/test_mcs_lock.cpp` | 5 test cases for MCS lock: single node, two-node contention, fairness (3 nodes), multiple locks, timeout |
| `tests/test_qspinlock.cpp` | 5 test cases for qspinlock: single node, two-node contention, fast-path verification, multiple locks, timeout |
| `tests/test_all_algorithms.cpp` | 5 comprehensive comparison tests across all 4 algorithms: basic functionality, fairness comparison, performance benchmark, high contention, stress test |

---

## Modified Files

### `CMakeLists.txt`

**Changes:**
- Added new source files: `src/mcs_global_lock.cpp`, `src/qspinlock_global_lock.cpp`
- Added new headers to `CXL_LOCK_HEADERS` list: `include/mcs_global_lock.h`, `include/qspinlock_global_lock.h`
- Added 3 new test executables: `test_mcs_lock`, `test_qspinlock`, `test_all_algorithms`

### `include/two_tier_lock.h`

**Changes:**
- Added two `#include` directives:
  ```cpp
  #include "mcs_global_lock.h"
  #include "qspinlock_global_lock.h"
  ```

### `src/two_tier_lock.cpp`

**Changes:**
- Added two `#include` directives for the new algorithm headers.
- Extended `poll_for_grant()` with two new `dynamic_cast` branches:
  - `McsGlobalLockAlgorithm`: polls by reading `McsNode->locked` (2=granted) and `waiting_lock`.
  - `QspinlockGlobalLockAlgorithm`: polls by reading `QspinNode->locked` (2=granted) and `waiting_lock`.

### `src/distributed_lock.cpp`

**Changes:**
- Added two `#include` directives for the new algorithm headers.
- Extended `initialize()` (Step 2) to accept `"mcs"`/`"MCS"` and `"qspinlock"`/`"QSPINLOCK"` algorithm names, constructing `McsGlobalLockAlgorithm` and `QspinlockGlobalLockAlgorithm` respectively.
- Extended lock manager initialization with corresponding `else if` branches for the two new algorithms.

### `src/faa_global_lock.cpp` (Bug Fixes)

**Bug Fix 1 - `release_lock()` not clearing `owner_node`:**
- **Problem:** `release_lock()` did not reset `entry->owner_node` to `INVALID_NODE_ID`. Consequently, `check_grant()` always saw `owner != INVALID` after the first grant and never granted the lock to subsequent waiters, causing infinite timeouts.
- **Fix:** Added `atomic_store` of `INVALID_NODE_ID` to `owner_node` in `release_lock()`, with proper `flush_cache`.

**Bug Fix 2 - `grant_lock()` incorrectly clearing `active` flag:**
- **Problem:** `grant_lock()` cleared `nt->active = 0`, but `poll_for_grant()` in `TwoTierLock` checks `active != 0` as part of its grant detection condition (`now_serving >= my_ticket && active != 0`). After `grant_lock` cleared `active`, the waiting node could never detect that it had been granted.
- **Fix:** Removed the `nt->active = 0` line from `grant_lock()`. The `active` flag is now only cleared in `release_lock()`, matching the poll condition in `TwoTierLock`.

---

## Architecture Changes Summary

### What Changed

1. **Algorithm registry** now has 4 algorithms instead of 2: `cas`, `faa`, `mcs`, `qspinlock`.
2. **`TwoTierLock::poll_for_grant()`** now uses `dynamic_cast` chains to detect which algorithm is in use. This is the existing pattern; it was extended by 2 more casts.
3. **`DistributedLockSystem`** now accepts two new `lock_algorithm` config strings.

### What Did NOT Change

- The `GlobalLockAlgorithm` interface is unchanged.
- The `LockAlgorithmRegistry` plugin mechanism is unchanged.
- `CasGlobalLockAlgorithm` is completely untouched.
- The shared memory layout of CAS and FAA algorithms is unchanged.
- The `LocalLock` (pthread_mutex) layer is unchanged.
- The `LockManager` scanning loop is unchanged.
- The node registry, CXL memory pool, and shared memory abstraction layers are unchanged.

---

## Test Results

| Test Suite | Cases | Passed | Failed |
|------------|-------|--------|--------|
| `test_basic_lock` | 8 | 8 | 0 |
| `test_contention` | 1 | 1 | 0 |
| `test_mcs_lock` | 5 | 5 | 0 |
| `test_qspinlock` | 5 | 5 | 0 |
| `test_all_algorithms` | 5 | 5 | 0 |

All existing tests continue to pass with no regressions.

---

## Known Limitations

1. **FAA high-contention behavior:** The FAA (ticket lock) algorithm relies on periodic lock-manager polling to advance `now_serving`. Under very high contention (many nodes, few locks), this can lead to timeouts because the lock manager may not scan fast enough to keep up with the release/grant rate. This is a fundamental design limitation of the polling-based approach, not a bug introduced by this change.
2. **qspinlock performance:** qspinlock is ~5x slower than CAS/FAA/MCS in the benchmark because its slow path involves more atomic operations and cache flushes. This is expected for a hybrid algorithm optimized for fairness over raw throughput.
