# CXL 分布式锁性能分析报告

## 测试环境

| 参数 | 值 |
|------|-----|
| CPU | Intel Xeon 6982P-C |
| 核心数 | **2 核** |
| 竞争实体 | 4 worker 进程 + 1 lock manager 线程 = **5 个** |
| 共享内存 | 文件 mmap (SOFTWARE_CONSISTENCY) |
| 锁管理器 scan 间隔 | 50us |

## 性能数据

| 算法 | 吞吐量 | 总耗时 | 超时次数 |
|------|--------|--------|----------|
| CAS | ~39.8 ops/s | ~1006ms | 0 |
| FAA | ~39.2 ops/s | ~1020ms | 0 |
| MCS | ~39.3 ops/s | ~1018ms | 0 |
| qspinlock | ~6.3 ops/s | ~6010ms | 2 |

## 瓶颈分析

### 瓶颈 1：Lock Manager Scan 延迟（核心瓶颈，影响所有算法）

锁管理器设计为每 50us 扫描一次共享内存：

```cpp
void LockManager::run() {
    while (!stop_requested_) {
        run_single_scan();  // ~1-2us
        std::this_thread::sleep_for(50us);  // 期望 sleep 50us
    }
}
```

**但在 2 核 / 5 竞争实体的高负载下：**

1. 4 个 worker 进程在 `poll_for_grant` 中做忙等待（adaptive backoff 的 PAUSE 阶段消耗 100% CPU）
2. Lock manager 线程 `sleep_for(50us)` 后唤醒，需要等待 CFS 调度
3. CFS 调度延迟 ≈ 3 个其他实体的时间片 ≈ **7ms**
4. **Effective scan 间隔 = 50us + 7ms ≈ 7.25ms**（比设计值慢 **145 倍**）

每次 grant 平均需要 3-4 个 scan 周期 ≈ **21-29ms**。

**计算验证：**
- 40 次操作 × 24ms (grant 延迟) = 960ms
- 加上临界区 40 × 0.5ms = 20ms
- 理论总时间 ≈ 980ms ≈ **实际 1006ms ✓**

### 瓶颈 2：poll_for_grant 的自旋开销

```cpp
for (;;) {
    // check grant: atomic_load + compare (~40ns)
    if (granted) break;
    // timeout check (~30ns)
    adaptive_backoff(++spin_count);  // PAUSE / yield / sleep
}
```

`adaptive_backoff` 分层策略：
- Phase 1 (1-100 次): `pause` 指令
- Phase 2 (101-1000 次): 10× `pause`
- Phase 3 (1001-10000 次): `yield()`
- Phase 4 (>10000 次): `sleep_for(10us)`

**问题**：在 2 核 / 5 实体的高负载下，即使 grant 已经就绪，`poll_for_grant` 仍要完成当前 backoff 阶段才能检测到。

- 10000 次 spin × 40ns = **400us 纯自旋**
- 然后进入 sleep 10us，调度延迟 2-7ms
- **每次 poll 最少浪费 2.5-7.4ms**

### 瓶颈 3：qspinlock 的 Cache Coherence 问题（仅 SOFTWARE_CONSISTENCY）

qspinlock 使用 MCS 链式传递锁。当 Node A 传递锁给 Node B 时：

```cpp
// Node A (release_lock)
succ->locked = 2;
shm->flush_cache(&succ->locked, sizeof(uint32_t));
```

`flush_cache` 使用 `CLFLUSH` 指令，但 **CLFLUSH 只驱逐当前 CPU 的 cache line，不会使其他 CPU 的 cache line 失效**。

Node B 在 `poll_for_grant` 中读取 `node->locked`：
```cpp
uint32_t node_locked = shm->atomic_load<uint32_t>(&node->locked, seq_cst);
```

`atomic_load` 在 x86 上编译为普通 `mov` 指令，可能从 Node B 的过期 cache 中读取旧值（1 = WAITING），看不到新值（2 = LOCKED）。

**结果**：Node B 长时间 poll 等待，直到 cache line 被某种原因驱逐（上下文切换），或直到 timeout。

**修复**：在 `flush_cache` 后添加 LOCK 前缀操作触发 MESI 跨 CPU cache 一致性：
```cpp
void flush_cache(...) {
    flush_cache_region(addr, size);  // CLFLUSH + MFENCE
    // LOCK-prefixed operation triggers MESI invalidation on other CPUs
    std::atomic_ref<uint32_t> ref(*static_cast<uint32_t*>(addr));
    ref.fetch_add(0, std::memory_order_seq_cst);
}
```

### 瓶颈 4：CAS 算法的顺序偏见

```cpp
NodeId CasGlobalLockAlgorithm::check_grant(...) {
    for (NodeId nid = 0; nid < node_count; ++nid) {  // 按 node_id 顺序
        if (val == WAITING) return nid;
    }
}
```

Node 0 永远优先获得锁，Node 3 永远最后。导致：
- Node 0 avg acquire = **179us**
- Node 3 avg acquire = **2310us**（13 倍差距）

## 优化建议

### 立即可做的优化（软件层面）

1. **减小锁管理器 scan 间隔**：从 50us 减小到 10us 或 1us
   ```cpp
   config.lock_manager_scan_interval_us = 10;  // 甚至 1
   ```

2. **优化 poll_for_grant 的 backoff 参数**：减小 spin 阈值，更快进入 yield/sleep
   ```cpp
   // 当前：10000 次 spin 后才 sleep
   // 建议：1000 次 spin 后就 sleep
   if (spin_count > 1000) {
       std::this_thread::sleep_for(1us);  // 用 1us 代替 10us
   }
   ```

3. **CAS check_grant 改为轮转扫描**：避免 Node 0 永远优先
   ```cpp
   static NodeId last_grantee = 0;
   for (NodeId offset = 0; offset < node_count; ++offset) {
       NodeId nid = (last_grantee + 1 + offset) % node_count;
       if (val == WAITING) { last_grantee = nid; return nid; }
   }
   ```

### 真实 CXL 硬件上的预期性能

在 `/dev/dax9.0` 真实 CXL 设备上（HARDWARE_CONSISTENCY 模式）：

1. **不需要 CLFLUSH**：CXL 硬件提供跨设备 cache 一致性
2. **不需要 poll_for_grant 忙等待**：可以使用 CXL 内存通知机制
3. **锁管理器 scan 可以更高效**：直接访问 CXL 内存，延迟更低

**预期性能提升 10-100 倍**（取决于 CXL 链路速度和具体硬件实现）。

## 修改汇总

为修复性能问题，对以下文件进行了修改：

| 文件 | 修改 | 原因 |
|------|------|------|
| `src/shared_memory.cpp` | `flush_cache` 添加 `fetch_add(0)` | 触发 MESI 跨 CPU cache 一致性 |
| `src/two_tier_lock.cpp` | poll_for_grant 添加 `invalidate_cache` | 确保读取前看到最新值 |
| `src/mcs_global_lock.cpp` | release_lock 添加 `invalidate_cache` | 确保读取 next_node 前看到最新值 |
| `src/qspinlock_global_lock.cpp` | release_lock 添加 `invalidate_cache` + sched_yield | 同上，避免 CPU 饿死 |
