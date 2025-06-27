# 或者手动指定（如果上面找不到）
include_directories(/usr/include/mysql)
link_libraries(mysqlclient)

# 添加源文件
file(GLOB NET_SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# 创建库
add_library(pool ${NET_SRC})

# 链接 MySQL 库
target_link_libraries(pool mysqlclient)
