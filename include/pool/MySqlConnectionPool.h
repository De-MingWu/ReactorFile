#ifndef MYSQLCONNECTIONPOOL_H
#define MYSQLCONNECTIONPOOL_H

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>

#include "MySqlConnection.h"


class ConnectionPool
{
public:
    // 获取连接池对象实例
    static ConnectionPool* GetConnectionPool();
    // 给外部提供一个接口，从连接池中获取一个可用的空闲连接
    std::shared_ptr<MySqlConnection> GetConnection();
    // 停止线程池
    void Stop();

private:
    std::string IP_; // 数据库ip地址
    unsigned short port_; // 数据库端口号
    std::string username_; // 登录用户名
    std::string password_; // 密码
    std::string dbname_;   // 连接的数据库名称
    int initSize_;    // 连接池初始连接量
    int maxSize_;     // 连接池最大连接量
    int maxIdleTime_; // 连接池最大空闲时间
    int connectionTimeOut_; //连接池获取连接的超时时间
    std::condition_variable cv; // 设置条件变量，用于连接生成者线程和连接消费线程的通信


    std::queue<MySqlConnection*> connectionQueue_; //存储MySQL连接队列
    std::mutex queueMutex_; // 维护连接队列的线程安全锁
    std::atomic_int connectionCnt_; // 记录已经创建连接的总数量

    std::thread produce_;  // 生产连接的线程
    std::thread scanner_;  // 扫描空闲连接的线程

    // 标志位用于停止线程
    bool stopFlag_;

    // 单例 构造函数私有化
    ConnectionPool();
    // 加载配置文件
    bool LoadConfigFile();
    // 运行在独立的线程中，专门负责生产新连接
    void ProduceConnectionTask();
    // 扫描超过maxIdleTime的空闲连接，用于连接回收
    void ScannerConnectionTask();
};


#endif //MYSQLCONNECTIONPOOL_H