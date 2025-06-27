# 基于Reactor网络库实现的个人云文件系统

## 1 介绍

基于C++11标准的**Reactor的高并发服务器通讯框架**基础上，实现的一个个人云文件系统。实现功能如下

+ 用户登录、注册
+ 文件上传、下载
+ 文件分享

## 2 目录结构

```
ReactorFile/
├── include     # 头文件部分
│   ├── base    # 一些公共的头文件
│   ├── http    # http请求 解析部分
│   ├── log     # 同步、异步日志实现
│   ├── net     # 网络核心
│   ├── pool    # 线程池、数据库连接池部分
│   └── service # 主要功能实现部分
├── mysql.ini   # 数据库连接池所需信息
└── src 	    # 函数实现
    ├── base
    ├── fileapp # 主文件入口部分
    ├── http
    ├── log
    ├── net
    ├── pool
    └── service
```

## 3 环境

WSL2024.04 

MySql 8，注意在mysql.ini更改成你的Mysql信息

json库，安装命令如下

```
sudo apt update
sudo apt install nlohmann-json3-dev
```

## 4 SQL数据库

数据库的详细设计参考以下内容：

1. 用户表(users)
2. 会话表(sessions)
3. 文件表(files)
4. 文件分享表(file_shares)

以下是完整的SQL语句：

```mysql
 -- 创建数据库
 CREATE DATABASE IF NOT EXISTS file_manager DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
 
 USE file_manager;
 
 -- 创建用户表
 CREATE TABLE IF NOT EXISTS users (
     id INT PRIMARY KEY AUTO_INCREMENT,
     username VARCHAR(50) NOT NULL UNIQUE,
     password VARCHAR(64) NOT NULL,
     email VARCHAR(100),
     created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
     updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
     INDEX idx_username (username)
 ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
 
 -- 创建会话表
 CREATE TABLE IF NOT EXISTS sessions (
     id INT PRIMARY KEY AUTO_INCREMENT,
     session_id VARCHAR(32) NOT NULL UNIQUE,
     user_id INT NOT NULL,
     username VARCHAR(50) NOT NULL,
     expire_time TIMESTAMP NOT NULL,
     created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
     INDEX idx_session_id (session_id),
     INDEX idx_user_id (user_id),
     INDEX idx_expire_time (expire_time),
     FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
 ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
 
 -- 创建文件表
 CREATE TABLE IF NOT EXISTS files (
     id INT PRIMARY KEY AUTO_INCREMENT,
     filename VARCHAR(255) NOT NULL,
     original_filename VARCHAR(255) NOT NULL,
     file_size BIGINT UNSIGNED NOT NULL,
     file_type VARCHAR(50),
     user_id INT NOT NULL,
     created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
     updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
     INDEX idx_filename (filename),
     INDEX idx_user_id (user_id),
     FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
 ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
 
 -- 创建文件分享表
 CREATE TABLE IF NOT EXISTS file_shares (
     id INT PRIMARY KEY AUTO_INCREMENT,
     file_id INT NOT NULL,
     owner_id INT NOT NULL,
     shared_with_id INT,
     share_type ENUM('private', 'public', 'protected', 'user') NOT NULL,
     share_code VARCHAR(32) NOT NULL,
     expire_time TIMESTAMP NULL,
     created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
     extract_code VARCHAR(6),
     INDEX idx_file_id (file_id),
     INDEX idx_owner_id (owner_id),
     INDEX idx_shared_with_id (shared_with_id),
     INDEX idx_share_code (share_code),
     INDEX idx_expire_time (expire_time),
     FOREIGN KEY (file_id) REFERENCES files(id) ON DELETE CASCADE,
     FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE,
     FOREIGN KEY (shared_with_id) REFERENCES users(id) ON DELETE CASCADE
 ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
 
```

表结构说明：

1. `users` 表：
   - 存储用户基本信息
   - 包含用户名、密码(哈希后)、邮箱等字段
   - 用户名设置为唯一索引
2. `sessions` 表：
   - 存储用户会话信息
   - 包含会话ID、用户ID、过期时间等字段
   - 设置了多个索引以提高查询效率
3. `files` 表：
   - 存储文件信息
   - 包含文件名、原始文件名、文件大小、文件类型等字段
   - 与用户表关联
4. `file_shares` 表：
   - 存储文件分享信息
   - 支持多种分享类型(私有、公开、受保护、指定用户)
   - 包含分享码、提取码、过期时间等字段
   - 与文件和用户表关联

## 5 编译和运行

```
git clone https://github.com/De-MingWu/ReactorFile.git

cd ReactorFile

mkdir build && cd build

cmake ..

make

./test 127.0.0.1 8888
```

在浏览器输入127.0.0.1:8888即可访问

补充tips：还有部分功能为实现，同时工作线程没启动，启动工作线程遇到的问题是，文件上传不了，感觉应该是文件分块上传，分配给了不同的连接造成，正在努力解决。
