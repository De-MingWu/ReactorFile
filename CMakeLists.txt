cmake_minimum_required(VERSION 3.28)
project(ReactorServer)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)

# 查找 nlohmann-json 库的头文件
include_directories(/usr/include)

# 查找 MySQL 头文件
include_directories(/usr/include/mysql)
link_libraries(mysqlclient)

# 添加 include 子目录
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/base
    ${PROJECT_SOURCE_DIR}/include/net
    ${PROJECT_SOURCE_DIR}/include/http
    ${PROJECT_SOURCE_DIR}/include/log
    ${PROJECT_SOURCE_DIR}/include/service
    ${PROJECT_SOURCE_DIR}/include/pool
)

# 添加子目录模块
add_subdirectory(src/base)
add_subdirectory(src/net)
add_subdirectory(src/http)
add_subdirectory(src/log)
add_subdirectory(src/service)
add_subdirectory(src/pool)

# 主程序入口
add_executable(test src/fileapp/main.cpp)

# 链接其他库
target_link_libraries(test base net http log service pthread pool mysqlclient stdc++fs)
