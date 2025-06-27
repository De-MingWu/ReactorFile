#ifndef MYSQLCONNECTION_H
#define MYSQLCONNECTION_H

#include <iostream>
#include <mysql.h>
#include <string>
#include <ctime>


// 数据库操作类
class MySqlConnection
{
private:
    MYSQL *conn_; // 表示和MySQL Server的一条连接
    clockid_t startTime_; // 连接进入空闲状态的起始时间
    
public:
    // 初始化数据库连接
    MySqlConnection();
    // 释放数据库连接资源
    ~MySqlConnection();
    // 连接数据库
    bool ConnectSQL(std::string ip, unsigned short port, std::string user, std::string password,
                 std::string dbname);
    // 更新操作 insert、delete、update
    int Update(std::string sql);
    // 查询操作 select
    MYSQL_RES* Query(std::string sql);
    // 刷新一下连接起始空闲时间点
    void RefreshAliveTime(){ startTime_ = clock(); }
    clock_t GetAliveTime() {return clock() - startTime_;}

    // 返回底层的 MYSQL* 连接
    MYSQL* GetRawConnection() { return conn_; }
};

#endif //MYSQLCONNECTION_H