# CMakeLists.txt —— 纯 CMake 构建（学习精简版）
#
# 原项目用 just + conan + CMake 三层，依赖多。这里只用最基础的 CMake，
# 手动把仓库自带的 IgH 静态库(libethercat.a)链接进来，不需要 conan/just。

cmake_minimum_required(VERSION 3.10)
project(kinco_ethercat_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 下位机可执行文件：把 controller/source 下四个 .cpp 编到一起
add_executable(controller_node
    controller/source/main.cpp
    controller/source/ecat_master.cpp
    controller/source/kinco_driver.cpp
    controller/source/socket_server.cpp
)

# 头文件搜索路径：我们自己的 include + 第三方 IgH 的 ecrt.h
target_include_directories(controller_node PRIVATE
    ${CMAKE_SOURCE_DIR}/controller/include
    ${CMAKE_SOURCE_DIR}/third_party/etherlab/include
)

# 链接真实的 IgH EtherCAT 主站静态库（保真依赖，不 mock）
target_link_libraries(controller_node
    ${CMAKE_SOURCE_DIR}/third_party/etherlab/lib/libethercat.a
    pthread   # 我们用了 std::thread
    rt        # clock_nanosleep 在部分系统需要 librt
)
