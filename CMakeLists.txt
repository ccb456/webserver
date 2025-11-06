# 指定 CMake 最低版本要求（根据你的开发环境调整，3.10 及以上较通用）
cmake_minimum_required(VERSION 3.10)

# 项目名称
project(Webserver)

# 设置 C++ 标准（建议使用 C++11 及以上，支持现代特性）
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# 编译选项：开启警告、优化等（可根据需求调整）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g")

# 指定头文件搜索路径（包含所有存放 .h 的目录）
include_directories(
    ${PROJECT_SOURCE_DIR}          # 当前目录（constance.h 等）
    ${PROJECT_SOURCE_DIR}/http     # http 模块头文件
    ${PROJECT_SOURCE_DIR}/pool/sqlConnPool  # SQL 连接池头文件
    ${PROJECT_SOURCE_DIR}/pool/threadPool   # 线程池头文件
    ${PROJECT_SOURCE_DIR}/timer    # 定时器模块头文件
)

# 收集所有源文件（.cpp）
set(SOURCES
    main.cpp
    ${PROJECT_SOURCE_DIR}/http/httpConn.cpp
    ${PROJECT_SOURCE_DIR}/pool/sqlConnPool/sqlConnPool.cpp
    ${PROJECT_SOURCE_DIR}/timer/timerHeap.cpp
)

# 生成可执行文件
add_executable(Webserver ${SOURCES})

# 链接依赖库（Web 服务器常用库）
# 1. 线程库（pthread，处理线程池）
# 2. 链接 MySQL 客户端库
target_link_libraries(Webserver pthread mysqlclient)


