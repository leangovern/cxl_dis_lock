# CXL 分布式锁系统 - API 参考手册

## 目录

1. [lock_types.h](#1-lock_typesh)
2. [lock_interface.h](#2-lock_interfaceh--lock_interfacecpp)
3. [shared_memory.h + shared_memory.cpp](#3-shared_memoryh--shared_memorycpp)
4. [node_registry.h + node_registry.cpp](#4-node_registryh--node_registrycpp)
5. [cxl_memory_pool.h + cxl_memory_pool.cpp](#5-cxl_memory_poolh--cxl_memory_poolcpp)
6. [local_lock.h + local_lock.cpp](#6-local_lockh--local_lockcpp)
7. [cas_global_lock.h + cas_global_lock.cpp](#7-cas_global_lockh--cas_global_lockcpp)
8. [faa_global_lock.h + faa_global_lock.cpp](#8-faa_global_lockh--faa_global_lockcpp)
9. [lock_manager.h + lock_manager.cpp](#9-lock_managerh--lock_managercpp)
10. [two_tier_lock.h + two_tier_lock.cpp](#10-two_tier_lockh--two_tier_lockcpp)
11. [distributed_lock.h + distributed_lock.cpp](#11-distributed_lockh--distributed_lockcpp)

---

## 1. lock_types.h

### 文件说明

全局类型定义头文件。声明锁系统使用的所有基础类型、枚举和常量。此文件被所有其他头文件包含，是系统的基石。

### 类型别名

| 别名 | 实际类型 | 说明 |
|------|----------|------|
| `NodeId` | `uint32_t` | 节点唯一标识符，范围 [0, MAX_NODE_COUNT) |
| `LockId` | `uint32_t` | 锁唯一标识符，范围 [0, global_lock_count) |
| `ShmOffset` | `uint64_t` | 共享内存区域内的偏移量（字节） |

### LockSlotState 枚举

```cpp
enum class LockSlotState : uint32_t {
    IDLE    = 0,  // 空闲状态，节点未竞争此锁
    WAITING = 1,  // 等待状态，节点请求获取锁
    LOCKED  = 2   // 锁定状态，节点已持有锁
};
```

CAS 算法中每个节点在全局锁表中的槽位状态：

| 状态值 | 含义 | 转换方向 |
|--------|------|----------|
| `IDLE` (0) | 节点未参与此锁的竞争 | WAITING（acquire 时） |
| `WAITING` (1) | 节点请求获取锁，等待 LockManager 授予 | LOCKED（被授予时）或 IDLE（取消时） |
| `LOCKED` (2) | 节点已成功获取锁，拥有临界区访问权 | IDLE（release 时） |

### ConsistencyMode 枚举

```cpp
enum class ConsistencyMode : uint32_t {
    SOFTWARE_CONSISTENCY = 0,  // 软件管理：每次写后 CLFLUSH + MFENCE
    HARDWARE_CONSISTENCY = 1   // 硬件一致性：依赖 CXL 硬件协议
};
```

### LockResult 枚举

```cpp
enum class LockResult : int32_t {
    SUCCESS           = 0,   // 操作成功
    ERR_INVALID_PARAM = -1,  // 参数无效（如 node_id 超出范围）
    ERR_SHM_FAULT     = -2,  // 共享内存访问错误
    ERR_TIMEOUT       = -3,  // 操作超时
    ERR_NOT_OWNER     = -4,  // 节点不是锁的持有者
    ERR_ALREADY_LOCKED= -5,  // 锁已被当前节点持有
    ERR_NODE_FAILED   = -6,  // 目标节点已失效
    ERR_NOT_INITIALIZED=-7,  // 系统未初始化
    ERR_UNKNOWN       = -99  // 未知错误
};
```

### 常量定义

| 常量 | 值 | 说明 |
|------|-----|------|
| `MAX_NODE_COUNT` | 64 | 系统支持的最大节点数 |
| `MAX_LOCK_COUNT` | 65536 | 系统支持的最大全局锁数量 |
| `MAX_DATA_KEY_SIZE` | 256 | 数据键的最大字节长度 |
| `NODE_INFO_SIZE` | 128 | 每个节点信息在共享内存中的占用字节数 |
| `HEARTBEAT_INTERVAL_MS` | 1000 | 心跳更新间隔（毫秒） |
| `HEARTBEAT_TIMEOUT_MS` | 3000 | 心跳超时阈值（毫秒） |
| `HEADER_SIZE` | 4096 | 共享内存头部区域大小（4KB 页对齐） |
| `CACHE_LINE_SIZE` | 64 | CPU 缓存行大小 |

---

## 2. lock_interface.h + lock_interface.cpp

### 文件说明

定义全局锁算法的抽象接口和算法注册表（工厂模式）。所有全局锁算法必须实现 `GlobalLockAlgorithm` 接口，并通过 `REGISTER_LOCK_ALGORITHM` 宏注册到 `LockAlgorithmRegistry` 中。

### GlobalLockAlgorithm 接口

```cpp
class GlobalLockAlgorithm {
public:
    virtual ~GlobalLockAlgorithm() = default;

    virtual void initialize(SharedMemoryRegion* shm,
                            ShmOffset offset,
                            uint32_t node_count,
                            uint32_t lock_count) = 0;

    virtual LockResult acquire_lock(NodeId node_id,
                                     LockId lock_id,
                                     SharedMemoryRegion* shm,
                                     ShmOffset offset) = 0;

    virtual LockResult release_lock(NodeId node_id,
                                     LockId lock_id,
                                     SharedMemoryRegion* shm,
                                     ShmOffset offset) = 0;

    virtual NodeId check_grant(LockId lock_id,
                                SharedMemoryRegion* shm,
                                ShmOffset offset,
                                uint32_t node_count) = 0;

    virtual bool grant_lock(NodeId node_id,
                            LockId lock_id,
                            SharedMemoryRegion* shm,
                            ShmOffset offset) = 0;

    virtual size_t get_state_size(uint32_t node_count,
                                   uint32_t lock_count) const = 0;

    virtual const char* get_name() const = 0;
};
```

#### initialize

| 项目 | 说明 |
|------|------|
| **功能** | 初始化算法在共享内存中的状态区域 |
| **参数** | `shm` - 共享内存区域指针；`offset` - 算法状态在共享内存中的起始偏移；`node_count` - 参与节点数；`lock_count` - 全局锁数量 |
| **返回值** | 无 |
| **线程安全** | 不安全，仅在系统初始化时由主线程调用一次 |
| **说明** | 此函数负责将共享内存的指定区域清零或设置为初始状态，建立算法所需的数据结构布局 |

#### acquire_lock

| 项目 | 说明 |
|------|------|
| **功能** | 节点声明对指定锁的竞争意图 |
| **参数** | `node_id` - 调用者节点 ID；`lock_id` - 目标锁 ID；`shm` - 共享内存区域指针；`offset` - 算法状态偏移 |
| **返回值** | `SUCCESS`（成功设置 WAITING 状态）；`ERR_INVALID_PARAM`（参数错误）；`ERR_ALREADY_LOCKED`（已持有锁） |
| **线程安全** | 安全，不同节点操作不同的共享内存槽位 |
| **说明** | CAS 算法中将对应槽位设为 WAITING；FAA 算法中执行 FAA 取号并将 ticket 写入节点区域 |

#### release_lock

| 项目 | 说明 |
|------|------|
| **功能** | 节点释放对指定锁的所有权 |
| **参数** | `node_id` - 调用者节点 ID；`lock_id` - 目标锁 ID；`shm` - 共享内存区域指针；`offset` - 算法状态偏移 |
| **返回值** | `SUCCESS`（成功释放）；`ERR_NOT_OWNER`（当前节点未持有此锁）；`ERR_INVALID_PARAM`（参数错误） |
| **线程安全** | 安全，节点只修改自己的槽位 |
| **说明** | CAS 算法中将对应槽位设为 IDLE；FAA 算法中清除节点的 ticket 信息 |

#### check_grant

| 项目 | 说明 |
|------|------|
| **功能** | 检查指定锁是否有等待的节点可以被授予（由 LockManager 调用） |
| **参数** | `lock_id` - 目标锁 ID；`shm` - 共享内存区域指针；`offset` - 算法状态偏移；`node_count` - 总节点数 |
| **返回值** | 应被授予锁的节点 ID（如无可授予节点返回 `UINT32_MAX`） |
| **线程安全** | 不安全，仅由 LockManager 单线程调用 |
| **说明** | CAS 算法扫描所有槽位，返回第一个 WAITING 状态的节点；FAA 算法找到最小 ticket 的等待节点 |

#### grant_lock

| 项目 | 说明 |
|------|------|
| **功能** | 将指定锁授予指定节点（由 LockManager 调用） |
| **参数** | `node_id` - 被授予锁的节点 ID；`lock_id` - 目标锁 ID；`shm` - 共享内存区域指针；`offset` - 算法状态偏移 |
| **返回值** | `true`（授予成功）；`false`（授予失败，如节点状态已改变） |
| **线程安全** | 不安全，仅由 LockManager 单线程调用 |
| **说明** | CAS 算法中将 WAITING 槽位 CAS 为 LOCKED；FAA 算法中设置 `now_serving` 和 `owner_node` |

#### get_state_size

| 项目 | 说明 |
|------|------|
| **功能** | 计算算法所需的共享内存状态区域大小 |
| **参数** | `node_count` - 节点数；`lock_count` - 锁数量 |
| **返回值** | 所需字节数（已包含缓存行对齐填充） |
| **线程安全** | 安全，只读计算 |
| **说明** | 用于 `CxlMemoryPool` 计算共享内存布局时确定各区域的偏移量 |

#### get_name

| 项目 | 说明 |
|------|------|
| **功能** | 返回算法名称（用于日志和调试） |
| **返回值** | C 风格字符串，如 `"cas"` 或 `"faa"` |
| **线程安全** | 安全，返回常量字符串 |

### LockAlgorithmRegistry 类

```cpp
class LockAlgorithmRegistry {
public:
    static LockAlgorithmRegistry& instance();

    template<typename T>
    void register_algorithm(const char* name);

    std::unique_ptr<GlobalLockAlgorithm> create(const char* name) const;
    bool has_algorithm(const char* name) const;
    std::vector<const char*> list_algorithms() const;

private:
    LockAlgorithmRegistry() = default;
    ~LockAlgorithmRegistry() = default;

    std::unordered_map<std::string,
                       std::function<std::unique_ptr<GlobalLockAlgorithm>()>> factories_;
    mutable std::shared_mutex mutex_;
};
```

#### instance

| 项目 | 说明 |
|------|------|
| **功能** | 获取算法注册表的单例实例 |
| **返回值** | `LockAlgorithmRegistry` 的引用 |
| **线程安全** | C++11 起安全（Meyer's Singleton） |

#### register_algorithm

| 项目 | 说明 |
|------|------|
| **功能** | 将算法类型注册到工厂中 |
| **参数** | `name` - 算法名称；模板参数 `T` - 算法类类型（必须继承 `GlobalLockAlgorithm` 且有默认构造函数） |
| **线程安全** | 安全，使用 `shared_mutex` 保护 |
| **典型调用** | `REGISTER_LOCK_ALGORITHM("cas", CasGlobalLockAlgorithm)` |

#### create

| 项目 | 说明 |
|------|------|
| **功能** | 根据名称创建算法实例 |
| **参数** | `name` - 算法名称 |
| **返回值** | `unique_ptr<GlobalLockAlgorithm>`（未找到返回 `nullptr`） |
| **线程安全** | 安全 |

#### has_algorithm / list_algorithms

| 项目 | 说明 |
|------|------|
| **功能** | 查询已注册的算法 |
| **线程安全** | 安全 |

### REGISTER_LOCK_ALGORITHM 宏

```cpp
#define REGISTER_LOCK_ALGORITHM(name, Class) \
    static struct Class##Registrar { \
        Class##Registrar() { \
            LockAlgorithmRegistry::instance().register_algorithm<Class>(name); \
        } \
    } g_##Class##_registrar;
```

此宏利用 C++ 全局对象的构造函数在程序启动时自动注册算法类，无需手动调用注册函数。

### lock_interface.cpp 实现说明

`lock_interface.cpp` 包含 `LockAlgorithmRegistry` 的实现：

| 函数 | 实现要点 |
|------|----------|
| `instance()` | Meyer's Singleton 模式，函数局部静态变量 |
| `register_algorithm()` | 使用 lambda 捕获类型信息，存入 `factories_` 哈希表 |
| `create()` | 查找 `factories_`，调用对应 lambda 创建实例 |
| `has_algorithm()` | 检查 `factories_` 中是否存在指定键 |
| `list_algorithms()` | 遍历 `factories_` 收集所有已注册的算法名称 |

---

## 3. shared_memory.h + shared_memory.cpp

### 文件说明

共享内存区域的抽象封装。支持 DAX 设备（`/dev/daxX.Y`，生产环境）和文件 backed 内存（测试环境）两种后端，提供统一的内存访问接口和缓存一致性原语。

### SharedMemoryRegion 类

```cpp
class SharedMemoryRegion {
public:
    SharedMemoryRegion();
    ~SharedMemoryRegion();

    // 生命周期管理
    bool init(const char* path, size_t size, bool create);
    bool init_file_backed(const char* filepath, size_t size, bool create);
    void cleanup();

    // 基本信息
    void* get_base_address() const;
    size_t get_size() const;
    bool is_valid() const;

    // 地址转换
    ShmOffset ptr_to_offset(const void* ptr) const;
    void* offset_to_ptr(ShmOffset offset) const;

    // 缓存一致性原语
    void flush_cache(const void* addr, size_t len) const;
    void memory_fence() const;
    void invalidate_cache(const void* addr, size_t len) const;

    // 原子操作（统一 CAS/FAA 接口）
    template<typename T>
    T atomic_load(const void* addr) const;

    template<typename T>
    void atomic_store(void* addr, T value) const;

    template<typename T>
    bool compare_exchange(void* addr, T* expected, T desired) const;

    template<typename T>
    T fetch_add(void* addr, T delta) const;

    // 一致性模式
    void set_consistency_mode(ConsistencyMode mode);
    ConsistencyMode get_consistency_mode() const;

private:
    void* base_address_;
    size_t size_;
    int fd_;
    bool is_dax_;
    ConsistencyMode consistency_mode_;
};
```

#### 构造函数 / 析构函数

| 项目 | 说明 |
|------|------|
| `SharedMemoryRegion()` | 初始化为未映射状态（`base_address_ = nullptr`，`size_ = 0`） |
| `~SharedMemoryRegion()` | 自动调用 `cleanup()`，确保资源不泄漏 |
| **线程安全** | 构造/析构不安全，不应与其他操作并发 |

#### init

| 项目 | 说明 |
|------|------|
| **功能** | 映射 DAX 设备到进程地址空间 |
| **参数** | `path` - DAX 设备路径（如 `/dev/dax0.0`）；`size` - 映射大小；`create` - 是否创建新映射 |
| **返回值** | `true`（成功）；`false`（失败，errno 包含错误码） |
| **线程安全** | 不安全，仅在初始化时调用一次 |
| **实现要点** | 打开设备文件 -> `mmap(MAP_SHARED)` -> 记录基地址和大小 |

#### init_file_backed

| 项目 | 说明 |
|------|------|
| **功能** | 映射文件 backed 共享内存（用于测试） |
| **参数** | `filepath` - 文件路径；`size` - 映射大小；`create` - 是否创建新文件 |
| **返回值** | `true`（成功）；`false`（失败） |
| **线程安全** | 不安全，仅在初始化时调用一次 |
| **实现要点** | `open(O_CREAT)` -> `ftruncate()` 设置文件大小 -> `mmap(MAP_SHARED)` |

#### cleanup

| 项目 | 说明 |
|------|------|
| **功能** | 解除映射并关闭文件描述符 |
| **线程安全** | 不安全，确保无其他线程正在访问共享内存 |
| **实现要点** | `munmap()` -> `close(fd)` -> 重置成员变量 |

#### get_base_address / get_size / is_valid

| 项目 | 说明 |
|------|------|
| **功能** | 访问共享内存的基本信息 |
| **线程安全** | 安全（初始化完成后只读） |
| **说明** | `is_valid()` 返回 `base_address_ != nullptr` |

#### ptr_to_offset / offset_to_ptr

| 项目 | 说明 |
|------|------|
| **功能** | 虚拟地址与共享内存偏移量的双向转换 |
| **参数** | `ptr` - 虚拟地址；`offset` - 共享内存偏移量 |
| **返回值** | 转换后的偏移量或虚拟地址 |
| **线程安全** | 安全（只读算术运算） |
| **说明** | `offset = (uintptr_t)ptr - (uintptr_t)base_address_` |

#### flush_cache

| 项目 | 说明 |
|------|------|
| **功能** | 将指定内存区域的缓存行强制写回主存（软件一致性模式） |
| **参数** | `addr` - 起始地址；`len` - 长度 |
| **线程安全** | 安全（每个线程独立调用） |
| **实现要点** | 逐缓存行调用 `_mm_clflushopt()`（如支持）或 `_mm_clflush()`，最后执行 `_mm_mfence()` |
| **硬件一致性模式** | 此函数为空操作（no-op） |

#### memory_fence

| 项目 | 说明 |
|------|------|
| **功能** | 全内存屏障，确保前后的内存操作不会被重排序 |
| **线程安全** | 安全 |
| **实现要点** | 调用 `_mm_mfence()`（x86）或 `__sync_synchronize()`（通用） |

#### invalidate_cache

| 项目 | 说明 |
|------|------|
| **功能** | 使指定内存区域的缓存行失效，强制下次读取从主存加载 |
| **参数** | `addr` - 起始地址；`len` - 长度 |
| **线程安全** | 安全 |
| **实现要点** | 逐缓存行调用 `_mm_clflush()` 使缓存失效（x86 无单独 invalidate 指令，用 CLFLUSH 模拟） |

#### atomic_load

| 项目 | 说明 |
|------|------|
| **功能** | 原子读取指定地址的值 |
| **参数** | `addr` - 源地址；模板参数 `T` - 值类型（`uint32_t` / `uint64_t`） |
| **返回值** | 读取的值 |
| **线程安全** | 安全，使用 `std::atomic<T>` 语义 |
| **实现要点** | `return __atomic_load_n(ptr, __ATOMIC_SEQ_CST)` |

#### atomic_store

| 项目 | 说明 |
|------|------|
| **功能** | 原子写入值到指定地址 |
| **参数** | `addr` - 目标地址；`value` - 写入值 |
| **线程安全** | 安全 |
| **实现要点** | `__atomic_store_n(ptr, value, __ATOMIC_SEQ_CST)`；软件一致性模式下随后调用 `flush_cache()` |

#### compare_exchange

| 项目 | 说明 |
|------|------|
| **功能** | 原子比较并交换（CAS） |
| **参数** | `addr` - 目标地址；`expected` - 预期值指针（失败时被更新为实际值）；`desired` - 目标值 |
| **返回值** | `true`（交换成功）；`false`（交换失败） |
| **线程安全** | 安全 |
| **实现要点** | `__atomic_compare_exchange_n(ptr, expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)` |

#### fetch_add

| 项目 | 说明 |
|------|------|
| **功能** | 原子获取并增加（FAA） |
| **参数** | `addr` - 目标地址；`delta` - 增量 |
| **返回值** | 增加前的旧值 |
| **线程安全** | 安全 |
| **实现要点** | `return __atomic_fetch_add(ptr, delta, __ATOMIC_SEQ_CST)`；软件一致性模式下随后调用 `flush_cache()` |

---

## 4. node_registry.h + node_registry.cpp

### 文件说明

节点注册服务，负责节点的注册、发现和健康监控。每个节点有一个本地配置（JSON 文件）和一份全局共享内存中的节点信息条目。

### NodeConfig 结构体

```cpp
struct NodeConfig {
    NodeId node_id;              // 本节点 ID
    char bind_address[32];       // 绑定 IP 地址
    uint16_t service_port;       // 服务端口号
    char shm_path[128];          // DAX 设备路径
    size_t shm_size;             // 共享内存大小
    uint32_t max_nodes;          // 最大参与节点数
    uint32_t global_lock_count;  // 全局锁数量
    uint32_t local_lock_count;   // 本地锁数量
    ConsistencyMode consistency_mode;  // 一致性模式
    char algorithm_name[16];     // 全局锁算法名称
};
```

### NodeGlobalInfo 结构体

```cpp
struct NodeGlobalInfo {
    NodeId node_id;              // 节点 ID
    char ip_address[16];         // IP 地址字符串
    uint32_t status;             // 0=inactive, 1=active, 2=suspected, 3=failed
    uint64_t last_heartbeat;     // 最后心跳时间戳（毫秒级 epoch）
    uint64_t register_time;      // 注册时间戳
    char reserved[92];           // 填充至 128 字节（缓存行对齐）
};
```

### NodeRegistry 类

```cpp
class NodeRegistry {
public:
    NodeRegistry();
    ~NodeRegistry();

    // 本地配置
    bool load_local_config(const char* config_path);
    const NodeConfig& get_local_config() const;

    // 共享内存注册
    bool register_in_shared_memory(SharedMemoryRegion* shm, ShmOffset registry_offset);
    bool unregister(SharedMemoryRegion* shm, ShmOffset registry_offset);

    // 心跳
    bool update_heartbeat(SharedMemoryRegion* shm, ShmOffset registry_offset);

    // 查询
    bool get_node_info(NodeId node_id,
                       NodeGlobalInfo* info,
                       SharedMemoryRegion* shm,
                       ShmOffset registry_offset) const;

    std::vector<NodeGlobalInfo> discover_active_nodes(
        SharedMemoryRegion* shm,
        ShmOffset registry_offset,
        uint32_t max_nodes) const;

    bool is_node_alive(NodeId node_id,
                       SharedMemoryRegion* shm,
                       ShmOffset registry_offset,
                       uint64_t timeout_ms) const;

private:
    NodeConfig local_config_;
    bool has_local_config_;
};
```

#### 构造函数 / 析构函数

| 项目 | 说明 |
|------|------|
| `NodeRegistry()` | 初始化 `has_local_config_ = false` |
| `~NodeRegistry()` | 无特殊操作 |

#### load_local_config

| 项目 | 说明 |
|------|------|
| **功能** | 从 JSON 配置文件读取本节点配置 |
| **参数** | `config_path` - JSON 文件路径 |
| **返回值** | `true`（成功）；`false`（文件不存在或格式错误） |
| **线程安全** | 不安全，仅初始化时调用 |
| **JSON 格式示例** | 见下方代码块 |

```json
{
    "node_id": 0,
    "bind_address": "192.168.1.10",
    "service_port": 18888,
    "shm_path": "/dev/dax0.0",
    "shm_size": 1073741824,
    "max_nodes": 64,
    "global_lock_count": 1024,
    "local_lock_count": 64,
    "consistency_mode": 0,
    "algorithm_name": "cas"
}
```

#### register_in_shared_memory

| 项目 | 说明 |
|------|------|
| **功能** | 将本节点信息写入共享内存的节点注册表 |
| **参数** | `shm` - 共享内存指针；`registry_offset` - 节点注册表在共享内存中的偏移 |
| **返回值** | `true`（成功）；`false`（槽位已被占用或参数错误） |
| **线程安全** | 不安全，注册阶段各节点应串行进行或使用独立槽位 |
| **实现要点** | 计算本节点对应的 `NodeGlobalInfo` 偏移 -> 填充信息 -> 写入共享内存 -> flush_cache |

#### unregister

| 项目 | 说明 |
|------|------|
| **功能** | 将本节点标记为非活跃状态 |
| **参数** | `shm` - 共享内存指针；`registry_offset` - 节点注册表偏移 |
| **返回值** | `true`（成功）；`false`（未注册或参数错误） |
| **线程安全** | 安全，只修改本节点槽位 |
| **实现要点** | 将 `status` 设为 0（inactive），更新时间戳 |

#### update_heartbeat

| 项目 | 说明 |
|------|------|
| **功能** | 更新本节点的心跳时间戳 |
| **参数** | `shm` - 共享内存指针；`registry_offset` - 节点注册表偏移 |
| **返回值** | `true`（成功） |
| **线程安全** | 安全，只修改本节点槽位的 `last_heartbeat` 字段 |
| **调用频率** | 应每隔 `HEARTBEAT_INTERVAL_MS`（默认 1000ms）调用一次 |
| **实现要点** | 读取当前时间 -> 写入 `last_heartbeat` -> flush_cache（仅 8 字节） |

#### get_node_info

| 项目 | 说明 |
|------|------|
| **功能** | 获取指定节点的全局信息 |
| **参数** | `node_id` - 目标节点；`info` - 输出缓冲区；`shm` - 共享内存指针；`registry_offset` - 偏移 |
| **返回值** | `true`（成功）；`false`（节点 ID 无效） |
| **线程安全** | 安全，只读操作 |

#### discover_active_nodes

| 项目 | 说明 |
|------|------|
| **功能** | 发现所有活跃节点 |
| **参数** | `shm` - 共享内存指针；`registry_offset` - 偏移；`max_nodes` - 最大节点数 |
| **返回值** | 包含所有 `status == 1`（active）节点的 `NodeGlobalInfo` 向量 |
| **线程安全** | 安全，只读扫描 |
| **实现要点** | 遍历注册表中所有槽位，筛选 `status == 1` 的条目 |

#### is_node_alive

| 项目 | 说明 |
|------|------|
| **功能** | 检查指定节点是否存活（基于心跳时间戳） |
| **参数** | `node_id` - 目标节点；`shm` - 共享内存指针；`registry_offset` - 偏移；`timeout_ms` - 超时阈值 |
| **返回值** | `true`（存活：当前时间 - last_heartbeat < timeout_ms）；`false`（可能已失效） |
| **线程安全** | 安全，只读操作 |
| **说明** | LockManager 使用此函数检测失效节点，清理其持有的锁 |

---

## 5. cxl_memory_pool.h + cxl_memory_pool.cpp

### 文件说明

CXL 共享内存池管理器，负责共享内存的整体布局规划、生命周期管理和底层刷新工具函数。它是 `SharedMemoryRegion` 的上层封装，增加了内存池级别的语义。

### CxlMemoryPool 类

```cpp
class CxlMemoryPool {
public:
    CxlMemoryPool();
    ~CxlMemoryPool();

    // 生命周期
    bool open(const char* dax_path, size_t size,
              uint32_t node_count, uint32_t lock_count,
              ConsistencyMode mode, bool create);
    bool open_file_backed(const char* filepath, size_t size,
                          uint32_t node_count, uint32_t lock_count,
                          ConsistencyMode mode, bool create);
    void close();

    // 访问器
    SharedMemoryRegion* get_shm();
    const PoolHeader* get_header() const;

    // 区域偏移量计算
    ShmOffset get_node_registry_offset() const;
    ShmOffset get_lock_table_offset() const;
    ShmOffset get_data_region_offset() const;

    // 工具函数
    static void flush_cache_line(void* addr);
    static void flush_cache_region(void* addr, size_t len);

private:
    SharedMemoryRegion shm_;
    PoolHeader* header_;
    bool is_open_;
};
```

### PoolHeader 结构体（定义在 cxl_memory_pool.h 中）

```cpp
struct PoolHeader {
    char magic[4];           // "CXL0"
    uint32_t version;        // 0x00010001
    uint32_t node_count;     // 最大节点数
    uint32_t lock_count;     // 全局锁数量
    uint32_t consistency_mode;  // 0=SW, 1=HW
    ShmOffset node_registry_offset;
    ShmOffset lock_table_offset;
    ShmOffset data_region_offset;
    char reserved[4072];     // 填充至 4096 字节
};
```

#### open

| 项目 | 说明 |
|------|------|
| **功能** | 打开 DAX 设备并初始化内存池布局 |
| **参数** | `dax_path` - DAX 设备路径；`size` - 总大小；`node_count` - 最大节点数；`lock_count` - 全局锁数；`mode` - 一致性模式；`create` - 是否创建新布局 |
| **返回值** | `true`（成功）；`false`（失败） |
| **线程安全** | 不安全，仅初始化时调用 |
| **实现要点** | 1. 调用 `shm_.init()` 映射 DAX 设备；2. 如 `create=true`，填充 `PoolHeader` 并计算各区域偏移量；3. 各区域按 64B 缓存行对齐 |

#### open_file_backed

| 项目 | 说明 |
|------|------|
| **功能** | 打开文件 backed 内存池（测试用途） |
| **参数** | 同 `open()`，但 `filepath` 为普通文件路径 |
| **线程安全** | 不安全 |
| **说明** | 内部调用 `shm_.init_file_backed()` |

#### close

| 项目 | 说明 |
|------|------|
| **功能** | 关闭内存池，解除映射 |
| **线程安全** | 不安全，确保所有操作已完成 |

#### get_shm

| 项目 | 说明 |
|------|------|
| **功能** | 获取底层的 `SharedMemoryRegion` 指针 |
| **返回值** | `SharedMemoryRegion*` |
| **说明** | 用于将共享内存传递给算法和注册表等组件 |

#### get_header / get_node_registry_offset / get_lock_table_offset / get_data_region_offset

| 项目 | 说明 |
|------|------|
| **功能** | 获取内存池头部信息及各区域偏移量 |
| **线程安全** | 安全（初始化完成后只读） |
| **说明** | 用于计算数据结构的共享内存地址 |

#### flush_cache_line

| 项目 | 说明 |
|------|------|
| **功能** | 刷新单个缓存行（64 字节） |
| **参数** | `addr` - 缓存行起始地址（自动对齐到 64 字节边界） |
| **说明** | 内联函数，使用 `_mm_clflush()` + `_mm_mfence()` |

#### flush_cache_region

| 项目 | 说明 |
|------|------|
| **功能** | 刷新指定内存区域的所有缓存行 |
| **参数** | `addr` - 起始地址；`len` - 长度 |
| **说明** | 逐缓存行调用 `flush_cache_line()` |

### 内存布局计算

```cpp
// open() 中的布局计算逻辑
size_t header_size = HEADER_SIZE;  // 4096
size_t registry_size = align_up(node_count * NODE_INFO_SIZE, CACHE_LINE_SIZE);
size_t lock_table_size = algorithm->get_state_size(node_count, lock_count);

header_->node_registry_offset = header_size;
header_->lock_table_offset = header_size + registry_size;
header_->data_region_offset = header_size + registry_size + lock_table_size;
```

---

## 6. local_lock.h + local_lock.cpp

### 文件说明

本地锁数组实现。每个节点在本地 DRAM 中维护一组 `pthread_mutex_t`，用于保护线程级并发。本地锁是第一层锁，确保同一节点内的多个线程不会同时竞争同一个全局锁。

### LocalLockArray 类

```cpp
class LocalLockArray {
public:
    LocalLockArray();
    ~LocalLockArray();

    bool initialize(uint32_t lock_count);
    void destroy();

    bool lock(uint32_t local_lock_id);
    bool unlock(uint32_t local_lock_id);
    bool try_lock(uint32_t local_lock_id);

    uint32_t get_lock_count() const;

private:
    pthread_mutex_t* mutexes_;
    uint32_t lock_count_;
    bool initialized_;
};
```

#### 构造函数 / 析构函数

| 项目 | 说明 |
|------|------|
| `LocalLockArray()` | 初始化为未初始化状态（`mutexes_ = nullptr`） |
| `~LocalLockArray()` | 如未调用 `destroy()`，自动调用以避免资源泄漏 |

#### initialize

| 项目 | 说明 |
|------|------|
| **功能** | 分配并初始化 pthread_mutex_t 数组 |
| **参数** | `lock_count` - 本地锁数量（建议值为全局锁数量的 1/16 ~ 1/64） |
| **返回值** | `true`（成功）；`false`（内存分配失败） |
| **线程安全** | 不安全，仅初始化时调用 |
| **实现要点** | `malloc()` 分配数组 -> 循环调用 `pthread_mutex_init()`（设置 `PTHREAD_MUTEX_ADAPTIVE_NP` 类型以优化自旋） |

#### destroy

| 项目 | 说明 |
|------|------|
| **功能** | 销毁所有互斥锁并释放内存 |
| **线程安全** | 不安全，确保所有锁已释放 |
| **实现要点** | 循环调用 `pthread_mutex_destroy()` -> `free(mutexes_)` |

#### lock

| 项目 | 说明 |
|------|------|
| **功能** | 阻塞式获取指定本地锁 |
| **参数** | `local_lock_id` - 本地锁索引 [0, lock_count) |
| **返回值** | `true`（成功）；`false`（参数无效） |
| **线程安全** | 安全，不同 `local_lock_id` 之间无竞争 |
| **说明** | 底层调用 `pthread_mutex_lock()`，会阻塞直到获取锁 |

#### unlock

| 项目 | 说明 |
|------|------|
| **功能** | 释放指定本地锁 |
| **参数** | `local_lock_id` - 本地锁索引 |
| **返回值** | `true`（成功）；`false`（参数无效或未持有锁） |
| **线程安全** | 安全 |
| **说明** | 底层调用 `pthread_mutex_unlock()` |

#### try_lock

| 项目 | 说明 |
|------|------|
| **功能** | 非阻塞式尝试获取本地锁 |
| **参数** | `local_lock_id` - 本地锁索引 |
| **返回值** | `true`（成功获取）；`false`（锁已被占用或参数无效） |
| **线程安全** | 安全 |
| **说明** | 底层调用 `pthread_mutex_trylock()` |

#### get_lock_count

| 项目 | 说明 |
|------|------|
| **返回值** | 本地锁数量 |
| **线程安全** | 安全（只读） |

### 本地锁与全局锁的映射

```
global_lock_id = 0, 1, 2, ..., N-1
local_lock_id  = global_lock_id % local_lock_count

例：global_lock_count = 1024, local_lock_count = 64
    global_lock_id 0   -> local_lock_id 0
    global_lock_id 1   -> local_lock_id 1
    ...
    global_lock_id 63  -> local_lock_id 63
    global_lock_id 64  -> local_lock_id 0  (多个全局锁共享一个本地锁)
    global_lock_id 65  -> local_lock_id 1
```

> 多个全局锁可以映射到同一个本地锁，这减少了本地锁的数量，但可能增加线程间阻塞的概率。建议 `local_lock_count` 为 `global_lock_count` 的 1/16 到 1/64。

---

## 7. cas_global_lock.h + cas_global_lock.cpp

### 文件说明

CAS（Compare-And-Swap）全局锁算法实现。使用二维数组记录每个锁的每个节点的状态（IDLE/WAITING/LOCKED），LockManager 按节点 ID 顺序扫描授予锁。此算法实现简单，但在高竞争下可能产生较多 CAS 冲突。

### 共享内存布局

```
+-----------------------------------------------------------+
| Lock 0: [Node 0 Slot] [Node 1 Slot] ... [Node N-1 Slot]   |
| Lock 1: [Node 0 Slot] [Node 1 Slot] ... [Node N-1 Slot]   |
| ...                                                       |
| Lock M-1: [Node 0] [Node 1] ... [Node N-1]               |
+-----------------------------------------------------------+
每个 Slot: uint32_t (LockSlotState: 0=IDLE, 1=WAITING, 2=LOCKED)
对齐：每行 (node_count * 4B) 向上对齐到 64B 缓存行
```

### CasGlobalLockAlgorithm 类

```cpp
class CasGlobalLockAlgorithm : public GlobalLockAlgorithm {
public:
    CasGlobalLockAlgorithm();

    void initialize(SharedMemoryRegion* shm,
                    ShmOffset offset,
                    uint32_t node_count,
                    uint32_t lock_count) override;

    LockResult acquire_lock(NodeId node_id,
                            LockId lock_id,
                            SharedMemoryRegion* shm,
                            ShmOffset offset) override;

    LockResult release_lock(NodeId node_id,
                            LockId lock_id,
                            SharedMemoryRegion* shm,
                            ShmOffset offset) override;

    NodeId check_grant(LockId lock_id,
                       SharedMemoryRegion* shm,
                       ShmOffset offset,
                       uint32_t node_count) override;

    bool grant_lock(NodeId node_id,
                    LockId lock_id,
                    SharedMemoryRegion* shm,
                    ShmOffset offset) override;

    size_t get_state_size(uint32_t node_count,
                          uint32_t lock_count) const override;

    const char* get_name() const override;

private:
    uint32_t slots_per_row_;  // 每行槽位数（向上对齐到缓存行）

    uint32_t* get_slot_ptr(SharedMemoryRegion* shm, ShmOffset offset,
                           LockId lock_id, NodeId node_id);
    uint32_t get_slot(SharedMemoryRegion* shm, ShmOffset offset,
                      LockId lock_id, NodeId node_id);
    void set_slot(SharedMemoryRegion* shm, ShmOffset offset,
                  LockId lock_id, NodeId node_id, uint32_t value);
};
```

#### 构造函数

| 项目 | 说明 |
|------|------|
| `CasGlobalLockAlgorithm()` | 初始化 `slots_per_row_ = 0` |

#### initialize

| 项目 | 说明 |
|------|------|
| **功能** | 清零所有锁槽位，设置为 IDLE 状态 |
| **参数** | 同接口定义 |
| **实现要点** | 计算 `slots_per_row_ = align_up(node_count, CACHE_LINE_SIZE / sizeof(uint32_t))` -> 遍历所有槽位，调用 `atomic_store(0)` |

#### acquire_lock

| 项目 | 说明 |
|------|------|
| **功能** | 将本节点对应槽位设为 WAITING |
| **参数** | 同接口定义 |
| **返回值** | `SUCCESS` / `ERR_INVALID_PARAM` / `ERR_ALREADY_LOCKED` |
| **实现要点** | 检查参数范围 -> 读取当前状态 -> 如已为 LOCKED 返回 `ERR_ALREADY_LOCKED` -> 否则 `atomic_store(slot, 1)`（WAITING）-> `flush_cache()` |
| **伪代码** | `slot[node_id] = WAITING; flush;` |

#### release_lock

| 项目 | 说明 |
|------|------|
| **功能** | 将本节点对应槽位设为 IDLE |
| **参数** | 同接口定义 |
| **返回值** | `SUCCESS` / `ERR_NOT_OWNER`（当前不为 LOCKED）/ `ERR_INVALID_PARAM` |
| **实现要点** | 检查参数 -> 读取当前状态 -> 如不为 LOCKED 返回 `ERR_NOT_OWNER` -> 否则 `atomic_store(slot, 0)`（IDLE）-> `flush_cache()` |
| **伪代码** | `if (slot[node_id] != LOCKED) return ERR_NOT_OWNER; slot[node_id] = IDLE; flush;` |

#### check_grant

| 项目 | 说明 |
|------|------|
| **功能** | 扫描指定锁的所有节点槽位，找到第一个 WAITING 节点 |
| **参数** | 同接口定义 |
| **返回值** | 可授予锁的节点 ID；无可授予节点返回 `UINT32_MAX` |
| **实现要点** | 遍历节点 0 到 N-1 -> 读取 `slot[i]` -> 如为 WAITING(1) 返回 i |
| **说明** | 按节点 ID 顺序授予，小 ID 节点有优先权 |

#### grant_lock

| 项目 | 说明 |
|------|------|
| **功能** | 将指定节点的 WAITING 槽位 CAS 为 LOCKED |
| **参数** | 同接口定义 |
| **返回值** | `true`（CAS 成功）；`false`（CAS 失败，节点状态已变） |
| **实现要点** | 读取当前值 -> `expected = WAITING` -> `desired = LOCKED` -> 执行 `compare_exchange()` -> 如成功调用 `flush_cache()` |

#### get_state_size

| 项目 | 说明 |
|------|------|
| **功能** | 计算 CAS 状态区域大小 |
| **返回值** | `lock_count * align_up(node_count * 4, 64)` |
| **说明** | 每行对齐到 64B 缓存行边界 |

#### get_name

| 项目 | 说明 |
|------|------|
| **返回值** | `"cas"` |

### CAS 算法状态机

```
          acquire_lock()              grant_lock()
IDLE    -------------> WAITING ------------------> LOCKED
           ^                                    |
           |         release_lock()             |
           +------------------------------------+
```

---

## 8. faa_global_lock.h + faa_global_lock.cpp

### 文件说明

FAA（Fetch-And-Add）全局锁算法实现。基于 ticket lock 思想：每个节点通过 FAA 获取一个唯一的 ticket 号码，LockManager 按照 ticket 号码从小到大顺序授予锁。此算法保证公平性（FIFO），且 FAA 操作的原子性避免了 CAS 冲突。

### 共享内存布局

```
+----------------------------------------------------------------+
| Lock 0 Entry (64B aligned)                                     |
|   next_ticket  : uint64_t  (下一个可用 ticket 号)               |
|   now_serving  : uint64_t  (当前正在服务的 ticket 号)            |
|   owner_node   : uint32_t  (持有锁的节点 ID)                    |
|   reserved     : 44 bytes                                      |
+----------------------------------------------------------------+
| Lock 0 Node Tickets (MAX_NODE_COUNT * 64B)                     |
|   [Node 0: ticket(8B) + padding(56B)]                          |
|   [Node 1: ticket(8B) + padding(56B)]                          |
|   ...                                                          |
+----------------------------------------------------------------+
| Lock 1 Entry (64B)                                             |
| Lock 1 Node Tickets (MAX_NODE_COUNT * 64B)                     |
+----------------------------------------------------------------+
| ...                                                            |
+----------------------------------------------------------------+
```

### FaaGlobalLockEntry 结构体

```cpp
struct alignas(64) FaaGlobalLockEntry {
    uint64_t next_ticket;    // 下一个可用的 ticket 号码
    uint64_t now_serving;    // 当前正在服务的 ticket 号码
    uint32_t owner_node;     // 持有锁的节点 ID
    char reserved[44];       // 填充至 64 字节
};
```

### FaaNodeTicket 结构体

```cpp
struct alignas(64) FaaNodeTicket {
    uint64_t ticket;         // 节点获取的 ticket 号码（0 表示无 ticket）
    char padding[56];        // 填充至 64 字节，避免伪共享
};
```

### FaaGlobalLockAlgorithm 类

```cpp
class FaaGlobalLockAlgorithm : public GlobalLockAlgorithm {
public:
    FaaGlobalLockAlgorithm();

    void initialize(SharedMemoryRegion* shm,
                    ShmOffset offset,
                    uint32_t node_count,
                    uint32_t lock_count) override;

    LockResult acquire_lock(NodeId node_id,
                            LockId lock_id,
                            SharedMemoryRegion* shm,
                            ShmOffset offset) override;

    LockResult release_lock(NodeId node_id,
                            LockId lock_id,
                            SharedMemoryRegion* shm,
                            ShmOffset offset) override;

    NodeId check_grant(LockId lock_id,
                       SharedMemoryRegion* shm,
                       ShmOffset offset,
                       uint32_t node_count) override;

    bool grant_lock(NodeId node_id,
                    LockId lock_id,
                    SharedMemoryRegion* shm,
                    ShmOffset offset) override;

    size_t get_state_size(uint32_t node_count,
                          uint32_t lock_count) const override;

    const char* get_name() const override;

private:
    FaaGlobalLockEntry* get_entry(SharedMemoryRegion* shm,
                                  ShmOffset offset,
                                  LockId lock_id);
    FaaNodeTicket* get_node_ticket(SharedMemoryRegion* shm,
                                   ShmOffset offset,
                                   LockId lock_id,
                                   NodeId node_id);
};
```

#### 构造函数

| 项目 | 说明 |
|------|------|
| `FaaGlobalLockAlgorithm()` | 无特殊初始化 |

#### initialize

| 项目 | 说明 |
|------|------|
| **功能** | 清零所有 Lock Entry 和 Node Ticket |
| **实现要点** | 遍历所有 `FaaGlobalLockEntry`，`atomic_store(0)` 所有字段 -> 遍历所有 `FaaNodeTicket`，`atomic_store(0)` |

#### acquire_lock

| 项目 | 说明 |
|------|------|
| **功能** | 通过 FAA 获取 ticket，将 ticket 写入本节点区域 |
| **返回值** | `SUCCESS` / `ERR_INVALID_PARAM` |
| **实现要点** | 对 `next_ticket` 执行 `fetch_add(1)` 获取 ticket -> 将 ticket 写入本节点的 `FaaNodeTicket` 区域 -> `flush_cache()` |
| **伪代码** | `ticket = FAA(&next_ticket, 1); node_ticket[node_id] = ticket; flush;` |

#### release_lock

| 项目 | 说明 |
|------|------|
| **功能** | 清除本节点的 ticket 信息 |
| **返回值** | `SUCCESS` / `ERR_INVALID_PARAM` |
| **实现要点** | 将本节点的 `ticket` 设为 0 -> `flush_cache()` |
| **说明** | FAA 算法中不需要检查持有者，因为 ticket 机制天然保证顺序 |

#### check_grant

| 项目 | 说明 |
|------|------|
| **功能** | 找到持有最小有效 ticket 的等待节点 |
| **返回值** | 应授予锁的节点 ID；无则返回 `UINT32_MAX` |
| **实现要点** | 读取 `now_serving` -> 遍历所有节点的 `ticket` -> 找到 `ticket > 0` 且最小者 -> 返回对应节点 ID |
| **说明** | 保证 FIFO 公平性，ticket 最小的节点优先获得锁 |

#### grant_lock

| 项目 | 说明 |
|------|------|
| **功能** | 更新 `now_serving` 为被授予节点的 ticket |
| **返回值** | `true`（成功） |
| **实现要点** | `atomic_store(&entry->now_serving, ticket)` -> `atomic_store(&entry->owner_node, node_id)` -> `flush_cache()` |

#### get_state_size

| 项目 | 说明 |
|------|------|
| **返回值** | `lock_count * (64 + align_up(node_count * 64, 64))` |
| **说明** | 每个锁：Entry(64B) + NodeTickets(node_count * 64B) |

#### get_name

| 项目 | 说明 |
|------|------|
| **返回值** | `"faa"` |

### FAA 算法工作示例

```
初始状态: next_ticket = 0, now_serving = 0, owner_node = MAX

Node 1 调用 acquire_lock:
  ticket = FAA(&next_ticket, 1) -> ticket = 0, next_ticket = 1
  node_ticket[1] = 0

Node 3 调用 acquire_lock:
  ticket = FAA(&next_ticket, 1) -> ticket = 1, next_ticket = 2
  node_ticket[3] = 1

Node 2 调用 acquire_lock:
  ticket = FAA(&next_ticket, 1) -> ticket = 2, next_ticket = 3
  node_ticket[2] = 2

LockManager 调用 check_grant:
  读取所有 node_ticket -> [1:0, 2:2, 3:1]
  最小有效 ticket = 0 (Node 1) -> 返回 Node 1

LockManager 调用 grant_lock(Node 1):
  now_serving = 0, owner_node = 1
  Node 1 检测到 now_serving == 自己的 ticket(0) -> 获得锁!

后续 LockManager 授予 Node 3 (ticket=1), Node 2 (ticket=2)...
```

---

## 9. lock_manager.h + lock_manager.cpp

### 文件说明

锁管理器是一个单例扫描线程，负责定期扫描全局锁状态并将锁授予等待中的节点。锁管理器通常运行在节点 0 上，但可以通过外部协调机制在其他节点上启动备份实例。

### LockManagerStats 结构体

```cpp
struct LockManagerStats {
    uint64_t total_scans;        // 总扫描次数
    uint64_t grants_issued;      // 授予的锁总数
    uint64_t grants_failed;      // 失败的授予尝试
    uint64_t orphaned_locks;     // 清理的孤儿锁数
    uint64_t failed_nodes;       // 检测到的失效节点数
    uint64_t active_nodes;       // 当前活跃节点数
    double avg_scan_time_us;     // 平均扫描时间（微秒）
};
```

### LockManager 类

```cpp
class LockManager {
public:
    static LockManager& instance();

    bool initialize(SharedMemoryRegion* shm,
                    ShmOffset lock_table_offset,
                    uint32_t node_count,
                    uint32_t lock_count,
                    std::unique_ptr<GlobalLockAlgorithm> algorithm);

    bool start();
    void stop();

    void run();                    // 阻塞式运行（在当前线程）
    void run_single_scan();        // 执行单次扫描（用于测试）

    LockManagerStats get_stats() const;
    void reset_stats();

    bool is_running() const;

private:
    LockManager() = default;
    ~LockManager();

    void scan_loop();              // 内部扫描循环

    SharedMemoryRegion* shm_;
    ShmOffset lock_table_offset_;
    uint32_t node_count_;
    uint32_t lock_count_;
    std::unique_ptr<GlobalLockAlgorithm> algorithm_;

    std::atomic<bool> running_;
    std::thread scan_thread_;

    mutable std::mutex stats_mutex_;
    LockManagerStats stats_;
};
```

#### instance

| 项目 | 说明 |
|------|------|
| **功能** | 获取锁管理器单例 |
| **线程安全** | C++11 起安全 |

#### initialize

| 项目 | 说明 |
|------|------|
| **功能** | 配置锁管理器的运行参数 |
| **参数** | `shm` - 共享内存；`lock_table_offset` - 锁表偏移；`node_count` / `lock_count` - 数量；`algorithm` - 全局锁算法实例 |
| **返回值** | `true`（成功） |
| **线程安全** | 不安全，仅初始化时调用 |
| **说明** | 必须在 `start()` 之前调用 |

#### start

| 项目 | 说明 |
|------|------|
| **功能** | 启动扫描线程 |
| **返回值** | `true`（成功启动）；`false`（已在运行或未初始化） |
| **线程安全** | 安全 |
| **实现要点** | `running_ = true` -> 创建 `std::thread(&LockManager::scan_loop, this)` |

#### stop

| 项目 | 说明 |
|------|------|
| **功能** | 停止扫描线程 |
| **线程安全** | 安全 |
| **实现要点** | `running_ = false` -> `join()` 等待线程结束 |

#### run

| 项目 | 说明 |
|------|------|
| **功能** | 在当前线程执行阻塞式扫描循环 |
| **说明** | 不创建新线程，直接调用 `scan_loop()`，用于单线程部署模式 |

#### run_single_scan

| 项目 | 说明 |
|------|------|
| **功能** | 执行单次扫描（用于测试和调试） |
| **实现要点** | 遍历所有 lock_id -> 对每个锁调用 `check_grant()` -> 如找到等待节点，调用 `grant_lock()` -> 更新统计信息 |

#### get_stats / reset_stats

| 项目 | 说明 |
|------|------|
| **功能** | 获取/重置性能统计 |
| **线程安全** | 安全，`stats_mutex_` 保护 |

#### is_running

| 项目 | 说明 |
|------|------|
| **返回值** | 扫描线程是否正在运行 |

### scan_loop 内部实现

```cpp
void LockManager::scan_loop() {
    while (running_.load()) {
        auto start = std::chrono::high_resolution_clock::now();

        for (LockId lock_id = 0; lock_id < lock_count_; ++lock_id) {
            // 1. 检查是否有等待节点
            NodeId waiting_node = algorithm_->check_grant(
                lock_id, shm_, lock_table_offset_, node_count_);

            if (waiting_node != UINT32_MAX) {
                // 2. 授予锁
                bool granted = algorithm_->grant_lock(
                    waiting_node, lock_id, shm_, lock_table_offset_);

                // 3. 更新统计
                std::lock_guard<std::mutex> lock(stats_mutex_);
                if (granted) {
                    stats_.grants_issued++;
                } else {
                    stats_.grants_failed++;
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        double scan_us = std::chrono::duration<double, std::micro>(end - start).count();

        // 更新平均扫描时间
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.total_scans++;
            stats_.avg_scan_time_us = (stats_.avg_scan_time_us * (stats_.total_scans - 1)
                                       + scan_us) / stats_.total_scans;
        }

        // 扫描间隔：自适应，最短 10us，最长 10ms
        std::this_thread::sleep_for(std::chrono::microseconds(
            std::min(10000, std::max(10, (int)(scan_us * 0.1)))));
    }
}
```

### 扫描优化策略

| 策略 | 说明 |
|------|------|
| 自适应扫描间隔 | 扫描耗时越长，间隔越长，减少 CPU 占用 |
| 批量授予 | 单次扫描遍历所有锁，避免频繁进入/退出循环 |
| 失效节点检测 | 扫描过程中检查等待节点的心跳，跳过失效节点 |
| 孤儿锁回收 | 如持有 LOCKED 的节点已失效，强制释放锁 |

---

## 10. two_tier_lock.h + two_tier_lock.cpp

### 文件说明

两层锁的实现层，组合了本地 DRAM 互斥锁和全局 CXL 共享内存锁。提供统一的 `acquire()` / `release()` 接口，内部处理两层锁的协调、自适应退避轮询和数据键到锁 ID 的哈希映射。

### BackoffState 枚举

```cpp
enum class BackoffState {
    SPIN,      // 忙等待自旋
    PAUSE,     // PAUSE 指令（提示 CPU 流水线）
    YIELD,     // sched_yield() 让出 CPU
    SLEEP      // usleep() 休眠
};
```

### TwoTierLock 类

```cpp
class TwoTierLock {
public:
    TwoTierLock();
    ~TwoTierLock();

    bool initialize(NodeId node_id,
                    LocalLockArray* local_locks,
                    SharedMemoryRegion* shm,
                    ShmOffset lock_table_offset,
                    GlobalLockAlgorithm* algorithm,
                    uint32_t global_lock_count,
                    uint32_t local_lock_count);

    // 锁操作
    LockResult acquire(LockId lock_id);
    LockResult release(LockId lock_id);
    LockResult try_acquire(LockId lock_id);

    // 数据键映射
    static LockId hash_u64_to_lock_id(uint64_t key, uint32_t global_lock_count);
    static LockId hash_bytes_to_lock_id(const uint8_t* data, size_t len,
                                        uint32_t global_lock_count);

    // 配置
    void set_max_backoff_us(uint32_t max_us);
    void set_spin_threshold(uint32_t threshold);

private:
    NodeId node_id_;
    LocalLockArray* local_locks_;
    SharedMemoryRegion* shm_;
    ShmOffset lock_table_offset_;
    GlobalLockAlgorithm* algorithm_;
    uint32_t global_lock_count_;
    uint32_t local_lock_count_;

    uint32_t max_backoff_us_;
    uint32_t spin_threshold_;

    LockId current_lock_id_;   // 当前持有的锁 ID（用于一致性检查）
    bool has_global_lock_;

    void adaptive_backoff(uint32_t& iteration, BackoffState& state);
    uint32_t map_to_local(LockId lock_id) const;
};
```

#### 构造函数 / 析构函数

| 项目 | 说明 |
|------|------|
| `TwoTierLock()` | 初始化成员为默认值 |
| `~TwoTierLock()` | 如仍持有锁，输出警告日志 |

#### initialize

| 项目 | 说明 |
|------|------|
| **功能** | 连接本地锁、共享内存和算法 |
| **参数** | `node_id` - 本节点 ID；`local_locks` - 本地锁数组；`shm` - 共享内存；`lock_table_offset` - 锁表偏移；`algorithm` - 全局锁算法；`global_lock_count` / `local_lock_count` - 锁数量 |
| **返回值** | `true`（成功）；`false`（参数无效） |
| **线程安全** | 不安全，初始化完成后方可调用 acquire/release |

#### acquire

| 项目 | 说明 |
|------|------|
| **功能** | 获取指定全局锁（阻塞式） |
| **参数** | `lock_id` - 目标锁 ID |
| **返回值** | `SUCCESS` / `ERR_INVALID_PARAM` / `ERR_TIMEOUT` |
| **线程安全** | 同一 `TwoTierLock` 实例不安全（应每线程一个实例）；不同实例安全 |
| **完整流程** | 见下表 |

| 步骤 | 操作 | 说明 |
|------|------|------|
| 1 | 检查参数范围 | `lock_id < global_lock_count` |
| 2 | `local_mutex_id = lock_id % local_lock_count` | 映射到本地锁 |
| 3 | `local_locks_->lock(local_mutex_id)` | 获取本地互斥锁 |
| 4 | `algorithm_->acquire_lock(node_id_, lock_id, shm_, lock_table_offset_)` | 设置全局 WAITING |
| 5 | 自适应退避轮询 | 循环读取全局状态直到 LOCKED |
| 6 | 返回 SUCCESS | 锁获取成功 |

#### release

| 项目 | 说明 |
|------|------|
| **功能** | 释放指定全局锁 |
| **参数** | `lock_id` - 要释放的锁 ID |
| **返回值** | `SUCCESS` / `ERR_INVALID_PARAM` / `ERR_NOT_OWNER` |
| **完整流程** | 见下表 |

| 步骤 | 操作 | 说明 |
|------|------|------|
| 1 | 检查参数范围 | |
| 2 | `algorithm_->release_lock(node_id_, lock_id, shm_, lock_table_offset_)` | 设置全局 IDLE |
| 3 | `local_mutex_id = lock_id % local_lock_count` | 计算本地锁 ID |
| 4 | `local_locks_->unlock(local_mutex_id)` | 释放本地互斥锁 |
| 5 | 返回 SUCCESS | |

#### try_acquire

| 项目 | 说明 |
|------|------|
| **功能** | 非阻塞式尝试获取锁 |
| **参数** | `lock_id` - 目标锁 ID |
| **返回值** | `SUCCESS`（获取成功）；`ERR_TIMEOUT`（锁不可用） |
| **说明** | 获取本地锁后设置 WAITING，检查一次全局状态，如不是 LOCKED 立即返回失败 |

#### hash_u64_to_lock_id

| 项目 | 说明 |
|------|------|
| **功能** | 将 64 位整数哈希为锁 ID |
| **参数** | `key` - 数据键；`global_lock_count` - 全局锁数量 |
| **返回值** | 锁 ID [0, global_lock_count) |
| **算法** | 位混淆：对 `key` 执行多次位移和异或混合高低位，然后取模 |

#### hash_bytes_to_lock_id

| 项目 | 说明 |
|------|------|
| **功能** | 将字节数组哈希为锁 ID |
| **参数** | `data` - 字节数组指针；`len` - 长度；`global_lock_count` - 全局锁数量 |
| **返回值** | 锁 ID [0, global_lock_count) |
| **算法** | FNV-1a 哈希后取模 |

#### adaptive_backoff

| 项目 | 说明 |
|------|------|
| **功能** | 根据轮询次数调整等待策略，减少 CPU 和总线争用 |
| **参数** | `iteration` - 当前轮询次数（引用，函数内递增）；`state` - 当前退避状态 |
| **状态转换** | 见下表 |

| 轮询次数 | 状态 | 行为 | 说明 |
|----------|------|------|------|
| 0 ~ spin_threshold (默认 100) | SPIN | 直接循环，CPU 空转 | 低延迟，适用于锁很快被授予的场景 |
| spin_threshold ~ spin_threshold*10 | PAUSE | 每次循环插入 `_mm_pause()` | 降低功耗，提示超线程 |
| spin_threshold*10 ~ spin_threshold*100 | YIELD | 调用 `sched_yield()` 让出 CPU | 避免长时间占用核心 |
| > spin_threshold*100 | SLEEP | `usleep()` 休眠，退避时间指数增长至 max_backoff_us | 长等待时彻底休眠 |

---

## 11. distributed_lock.h + distributed_lock.cpp

### 文件说明

分布式锁系统的主 API 类，采用 PImpl（Pointer to Implementation）模式封装所有内部组件。应用层通过此类与整个分布式锁系统交互，无需了解内部的两层锁结构和算法细节。

### DistributedLockSystem::Config 结构体

```cpp
struct Config {
    // 节点配置
    NodeId node_id;              // 本节点 ID
    const char* config_path;     // node_config.json 路径

    // 共享内存
    const char* shm_path;        // DAX 设备路径（生产环境）
    size_t shm_size;             // 共享内存大小
    bool use_file_backed;        // 是否使用文件 backed（测试）
    const char* file_path;       // 文件路径（use_file_backed=true 时使用）

    // 锁配置
    const char* algorithm_name;  // "cas" 或 "faa"
    uint32_t global_lock_count;  // 全局锁数量
    uint32_t local_lock_count;   // 本地锁数量

    // 一致性
    ConsistencyMode consistency_mode;

    // LockManager
    bool enable_lock_manager;    // 本节点是否运行 LockManager

    // 高级选项
    uint32_t max_backoff_us;     // 最大退避时间（微秒）
    uint32_t spin_threshold;     // 自旋阈值
};
```

### DistributedLockSystem 类

```cpp
class DistributedLockSystem {
public:
    DistributedLockSystem();
    ~DistributedLockSystem();

    // 生命周期
    bool initialize(const Config& config);
    void shutdown();
    bool is_initialized() const;

    // 锁操作（通过锁 ID）
    LockResult acquire_lock(LockId lock_id);
    LockResult release_lock(LockId lock_id);
    LockResult try_lock(LockId lock_id);

    // 锁操作（通过数据键）
    LockResult acquire_data_lock(uint64_t data_key);
    LockResult release_data_lock(uint64_t data_key);
    LockResult acquire_data_lock(const uint8_t* data, size_t len);
    LockResult release_data_lock(const uint8_t* data, size_t len);

    // 批量操作
    LockResult acquire_locks(const LockId* lock_ids, size_t count,
                             int* results);
    LockResult release_locks(const LockId* lock_ids, size_t count,
                             int* results);

    // 健康监控（预留接口）
    bool check_node_health(NodeId node_id) const;
    bool initiate_fault_recovery(NodeId failed_node);

    // 统计信息
    LockManagerStats get_lock_manager_stats() const;
    void print_stats() const;

private:
    class Impl;                  // 前置声明实现类
    std::unique_ptr<Impl> impl_; // PImpl 指针
};
```

#### 构造函数 / 析构函数

| 项目 | 说明 |
|------|------|
| `DistributedLockSystem()` | 创建 `Impl` 实例 |
| `~DistributedLockSystem()` | 如未调用 `shutdown()`，自动清理 |

#### initialize

| 项目 | 说明 |
|------|------|
| **功能** | 初始化整个分布式锁系统 |
| **参数** | `config` - 配置结构体 |
| **返回值** | `true`（成功）；`false`（失败，日志包含详细原因） |
| **线程安全** | 不安全，整个生命周期只调用一次 |
| **内部流程** | 见下表 |

| 步骤 | 组件 | 操作 |
|------|------|------|
| 1 | `NodeRegistry` | `load_local_config(config.config_path)` 读取本地配置 |
| 2 | `CxlMemoryPool` | `open()` 或 `open_file_backed()` 映射共享内存 |
| 3 | `LockAlgorithmRegistry` | `create(config.algorithm_name)` 创建算法实例 |
| 4 | `CxlMemoryPool` | 如 `create=true`，初始化 `PoolHeader` 和各区域偏移 |
| 5 | 算法 | `initialize()` 初始化锁状态区域 |
| 6 | `NodeRegistry` | `register_in_shared_memory()` 注册本节点 |
| 7 | `LocalLockArray` | `initialize(config.local_lock_count)` 创建本地互斥锁 |
| 8 | `TwoTierLock` | `initialize()` 连接所有组件 |
| 9 | `LockManager`（如启用）| `initialize()` + `start()` 启动扫描线程 |

#### shutdown

| 项目 | 说明 |
|------|------|
| **功能** | 关闭系统，释放所有资源 |
| **线程安全** | 不安全，确保所有锁操作已完成 |
| **内部流程** | 停止 LockManager -> 注销节点 -> 销毁本地锁 -> 关闭内存池 |

#### is_initialized

| 项目 | 说明 |
|------|------|
| **返回值** | 系统是否已完成初始化 |
| **线程安全** | 安全 |

#### acquire_lock

| 项目 | 说明 |
|------|------|
| **功能** | 获取指定 ID 的全局锁 |
| **参数** | `lock_id` - 锁 ID [0, global_lock_count) |
| **返回值** | `SUCCESS` / 错误码 |
| **线程安全** | 每线程应持有独立的 `DistributedLockSystem` 实例，或确保不同时调用 |
| **说明** | 委托给 `TwoTierLock::acquire()` |

#### release_lock

| 项目 | 说明 |
|------|------|
| **功能** | 释放指定 ID 的全局锁 |
| **参数** | `lock_id` - 锁 ID |
| **返回值** | `SUCCESS` / `ERR_NOT_OWNER` / 错误码 |
| **说明** | 委托给 `TwoTierLock::release()` |

#### try_lock

| 项目 | 说明 |
|------|------|
| **功能** | 非阻塞式尝试获取锁 |
| **参数** | `lock_id` - 锁 ID |
| **返回值** | `SUCCESS`（获取成功）；`ERR_TIMEOUT`（锁不可用） |
| **说明** | 委托给 `TwoTierLock::try_acquire()` |

#### acquire_data_lock / release_data_lock (uint64_t 版本)

| 项目 | 说明 |
|------|------|
| **功能** | 通过 64 位数据键获取/释放锁 |
| **参数** | `data_key` - 数据键（如内存地址、对象 ID） |
| **返回值** | 同 `acquire_lock` / `release_lock` |
| **内部实现** | `lock_id = hash_u64_to_lock_id(data_key, global_lock_count)` -> 调用 `acquire_lock(lock_id)` |
| **典型用途** | 保护哈希表中的某个 bucket：`acquire_data_lock(bucket_index)` |

#### acquire_data_lock / release_data_lock (字节数组版本)

| 项目 | 说明 |
|------|------|
| **功能** | 通过字节数组数据键获取/释放锁 |
| **参数** | `data` - 字节数组指针；`len` - 长度 |
| **返回值** | 同上 |
| **内部实现** | `lock_id = hash_bytes_to_lock_id(data, len, global_lock_count)` |
| **典型用途** | 保护 KV Store 中的某个 Key：`acquire_data_lock(key.data(), key.size())` |

#### acquire_locks / release_locks

| 项目 | 说明 |
|------|------|
| **功能** | 批量获取/释放多个锁 |
| **参数** | `lock_ids` - 锁 ID 数组；`count` - 数量；`results` - 每个锁的结果缓冲区 |
| **返回值** | `SUCCESS`（全部成功）；`ERR_INVALID_PARAM`（参数错误） |
| **说明** | 按锁 ID 排序后依次获取，避免死锁 |
| **注意事项** | 批量获取时应始终按相同顺序获取锁，避免循环等待 |

#### check_node_health

| 项目 | 说明 |
|------|------|
| **功能** | 检查指定节点是否存活（预留接口） |
| **参数** | `node_id` - 目标节点 |
| **返回值** | `true`（存活）；`false`（可能已失效） |
| **说明** | 当前实现委托给 `NodeRegistry::is_node_alive()` |

#### initiate_fault_recovery

| 项目 | 说明 |
|------|------|
| **功能** | 对失效节点发起故障恢复（预留接口） |
| **参数** | `failed_node` - 失效节点 ID |
| **返回值** | `true`（恢复成功）；`false`（失败） |
| **说明** | 当前为占位实现，未来版本将支持：清理失效节点的 WAITING/LOCKED 状态、选举新的 LockManager 等 |

#### get_lock_manager_stats / print_stats

| 项目 | 说明 |
|------|------|
| **功能** | 获取/打印 LockManager 的性能统计 |
| **返回值** | `LockManagerStats` 结构体 |
| **说明** | 仅在启用了 LockManager 的节点上有意义 |

### 使用示例

```cpp
// 1. 配置并初始化
DistributedLockSystem::Config config = {};
config.node_id = 1;
config.config_path = "/etc/cxl_lock/node_config.json";
config.shm_path = "/dev/dax0.0";
config.shm_size = 1024ULL * 1024 * 1024;  // 1GB
config.algorithm_name = "faa";
config.global_lock_count = 1024;
config.local_lock_count = 64;
config.consistency_mode = ConsistencyMode::SOFTWARE_CONSISTENCY;
config.enable_lock_manager = false;  // 节点 1 不运行 LockManager

DistributedLockSystem lock_sys;
if (!lock_sys.initialize(config)) {
    fprintf(stderr, "Failed to initialize lock system\n");
    return 1;
}

// 2. 通过锁 ID 获取/释放
LockResult r = lock_sys.acquire_lock(42);
if (r == LockResult::SUCCESS) {
    // 临界区操作...
    lock_sys.release_lock(42);
}

// 3. 通过数据键获取/释放
uint64_t data_key = 0xDEADBEEF;
r = lock_sys.acquire_data_lock(data_key);
if (r == LockResult::SUCCESS) {
    // 操作 data_key 对应的数据...
    lock_sys.release_data_lock(data_key);
}

// 4. 批量获取（避免死锁）
LockId locks[] = {10, 5, 20};
int results[3];
lock_sys.acquire_locks(locks, 3, results);
// 使用完批量释放
lock_sys.release_locks(locks, 3, results);

// 5. 关闭
lock_sys.shutdown();
```

---

## 附录 A：线程安全总结

| 类 | 线程安全级别 | 说明 |
|------|-------------|------|
| `SharedMemoryRegion` | 读安全，写需外部同步 | 原子操作函数线程安全，init/cleanup 不安全 |
| `NodeRegistry` | 查询安全，注册不安全 | 心跳更新安全（只改本节点槽位） |
| `CxlMemoryPool` | 读安全 | open/close 不安全 |
| `LocalLockArray` | 完全安全 | 每个锁独立的 pthread_mutex |
| `CasGlobalLockAlgorithm` | 节点间安全 | 每个节点修改自己的槽位 |
| `FaaGlobalLockAlgorithm` | 节点间安全 | FAA 原子操作保证无冲突 |
| `LockManager` | 内部安全 | 单线程扫描，stats 有 mutex 保护 |
| `TwoTierLock` | 实例级不安全 | 每线程应持有独立实例 |
| `DistributedLockSystem` | 实例级不安全 | 每线程应持有独立实例 |

## 附录 B：错误码速查表

| 错误码 | 值 | 触发场景 |
|--------|-----|----------|
| `SUCCESS` | 0 | 操作成功完成 |
| `ERR_INVALID_PARAM` | -1 | 节点 ID / 锁 ID 超出范围 |
| `ERR_SHM_FAULT` | -2 | 共享内存访问越界或设备错误 |
| `ERR_TIMEOUT` | -3 | 操作超时（try_lock 时） |
| `ERR_NOT_OWNER` | -4 | 释放不属于本节点的锁 |
| `ERR_ALREADY_LOCKED` | -5 | 重复获取已持有的锁 |
| `ERR_NODE_FAILED` | -6 | 目标节点已失效 |
| `ERR_NOT_INITIALIZED` | -7 | 系统未初始化 |
| `ERR_UNKNOWN` | -99 | 未知内部错误 |

## 附录 C：性能调优建议

| 参数 | 建议值 | 说明 |
|------|--------|------|
| `global_lock_count` | 数据分区数的 2~4 倍 | 减少锁争用 |
| `local_lock_count` | `global_lock_count / 16` | 平衡本地锁内存和线程阻塞 |
| `max_backoff_us` | 1000~10000 | 长等待时休眠时间 |
| `spin_threshold` | 100~1000 | 根据锁获取延迟调整 |
| `HEARTBEAT_INTERVAL_MS` | 500~2000 | 网络稳定时可增大 |
| 算法选择 | 高竞争用 FAA，低竞争用 CAS | FAA 公平性更好 |
| 一致性模式 | 有硬件一致性时用 HW | 省去 CLFLUSH 开销 |

---

*文档版本: 1.0 | API 版本: 1.0 | 最后更新: 2024*
