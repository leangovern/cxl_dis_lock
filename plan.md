# CXL Distributed Lock System - 执行计划

## 论文核心理解
TraCT proposes a two-tier inter-node locking mechanism:
1. **Local Tier**: Per-node DRAM-resident mutex locks (e.g., pthread_mutex). At most one thread per node contends for the global lock.
2. **Global Tier**: A fixed-size global lock array stored in CXL shared memory. Each entry has per-node slots with states I(Idle)/W(Waiting)/L(Lock granted).
3. **Lock Manager**: A dedicated thread that scans global locks and grants lock to one waiting node at a time.

## 执行阶段

### Stage 1: Architecture Design & Code Implementation
基于论文思想，设计并实现一个通用的、可扩展的分布式锁系统：
- **Lock Interface**: 抽象锁接口，支持CAS/FAA等不同全局锁实现
- **Two-Tier Lock**: 本地互斥锁 + 全局锁（CAS/FAA可替换）
- **Lock Manager**: 单线程管理全局锁队列
- **Shared Memory**: DAX设备映射，模拟环境用文件夹
- **Consistency**: clflush模拟（带硬件一致性开关）
- **Node Registration**: 基于配置文件的服务节点注册
- **Data-to-Lock Mapping**: 静态分配 + 哈希映射 + 二层解耦

### Stage 2: Test Programs
- **功能测试**: 测试分布式锁的基本功能和边界情况
- **业务场景模拟**: 多节点竞争同一数据资源的模拟

### Stage 3: Documentation & Diagrams
- 架构图、流程图
- 所有.h和.cpp文件的函数说明

## 文件结构
```
cxl_distributed_lock/
├── include/
│   ├── lock_interface.h      # 锁抽象接口
│   ├── cas_global_lock.h     # CAS全局锁实现
│   ├── faa_global_lock.h     # FAA全局锁实现
│   ├── local_lock.h          # 本地互斥锁
│   ├── two_tier_lock.h       # 二层锁整合
│   ├── lock_manager.h        # 锁管理器
│   ├── shared_memory.h       # 共享内存管理
│   ├── node_registry.h       # 节点注册
│   ├── cxlmemory_pool.h      # CXL内存池
│   └── distributed_lock.h    # 对外统一接口
├── src/
│   ├── cas_global_lock.cpp
│   ├── faa_global_lock.cpp
│   ├── local_lock.cpp
│   ├── two_tier_lock.cpp
│   ├── lock_manager.cpp
│   ├── shared_memory.cpp
│   ├── node_registry.cpp
│   ├── cxlmemory_pool.cpp
│   └── distributed_lock.cpp
├── tests/
│   ├── test_basic_lock.cpp   # 基础功能测试
│   └── test_contention.cpp   # 竞争场景测试
├── CMakeLists.txt
├── node_config.json          # 节点配置文件示例
└── docs/
    ├── architecture.md       # 架构说明
    └── flowcharts.md         # 流程图
```
