#include "MySqlConnection.h"
#include "Log.h"

MySqlConnection::MySqlConnection()
{
    conn_ = mysql_init(nullptr);
}
// 释放数据库连接资源
MySqlConnection::~MySqlConnection()
{
    if (conn_ != nullptr)
        mysql_close(conn_);
}
// 连接数据库
bool MySqlConnection::ConnectSQL(std::string ip, unsigned short port, std::string user, std::string password,
                            std::string dbname)
{
    MYSQL *p = mysql_real_connect(conn_, ip.c_str(), user.c_str(),
                                  password.c_str(), dbname.c_str(), port, nullptr, 0);
    return p != nullptr;
}
// 更新操作 insert、delete、update
int MySqlConnection::Update(std::string sql)
{
    if (mysql_query(conn_, sql.c_str()))
    {
        LOG_ERROR << "更新失败：" << sql;
        exit(0);
    }
    return static_cast<int>(mysql_affected_rows(conn_));
}

// 查询操作 select
MYSQL_RES* MySqlConnection::Query(std::string sql)
{
    if (mysql_query(conn_, sql.c_str()))
    {
        LOG_ERROR << "查询失败:" << sql;
        return nullptr;
    }
    return mysql_store_result(conn_);
}

