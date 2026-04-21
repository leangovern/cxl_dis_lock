cmake_minimum_required(VERSION 3.14)
project(CxlDistributedLock VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files
set(CXL_LOCK_SOURCES
    src/lock_interface.cpp
    src/shared_memory.cpp
    src/node_registry.cpp
    src/cxl_memory_pool.cpp
    src/local_lock.cpp
    src/cas_global_lock.cpp
    src/faa_global_lock.cpp
    src/mcs_global_lock.cpp
    src/qspinlock_global_lock.cpp
    src/lock_manager.cpp
    src/two_tier_lock.cpp
    src/distributed_lock.cpp
)

# Header files (for IDE support)
set(CXL_LOCK_HEADERS
    include/lock_types.h
    include/lock_interface.h
    include/shared_memory.h
    include/node_registry.h
    include/cxl_memory_pool.h
    include/local_lock.h
    include/cas_global_lock.h
    include/faa_global_lock.h
    include/mcs_global_lock.h
    include/qspinlock_global_lock.h
    include/lock_manager.h
    include/two_tier_lock.h
    include/distributed_lock.h
)

# Main library
add_library(cxl_lock STATIC ${CXL_LOCK_SOURCES} ${CXL_LOCK_HEADERS})
target_include_directories(cxl_lock PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(cxl_lock PUBLIC pthread)

# Enable warnings
target_compile_options(cxl_lock PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Debug>:-O0 -g3>
    $<$<CONFIG:Release>:-O3>
)

# Test: Basic lock test
add_executable(test_basic_lock tests/test_basic_lock.cpp)
target_link_libraries(test_basic_lock PRIVATE cxl_lock)

# Test: Contention simulation
add_executable(test_contention tests/test_contention.cpp)
target_link_libraries(test_contention PRIVATE cxl_lock)

# Test: All algorithms comparison
add_executable(test_all_algorithms tests/test_all_algorithms.cpp)
target_link_libraries(test_all_algorithms PRIVATE cxl_lock)

add_executable(test_all_algorithms_cxl_dax tests/test_all_algorithms_cxl_dax.cpp)
target_link_libraries(test_all_algorithms_cxl_dax PRIVATE cxl_lock)

# Test: MCS lock algorithm
add_executable(test_mcs_lock tests/test_mcs_lock.cpp)
target_link_libraries(test_mcs_lock PRIVATE cxl_lock)

# Test: qspinlock algorithm
add_executable(test_qspinlock tests/test_qspinlock.cpp)
target_link_libraries(test_qspinlock PRIVATE cxl_lock)

# Install targets
install(TARGETS cxl_lock
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CXL_LOCK_HEADERS} DESTINATION include/cxl_lock)