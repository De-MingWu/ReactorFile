#include "MySqlConnectionPool.h"
#include "Log.h"

// 初始化连接池
ConnectionPool::ConnectionPool()
{
    //加载配置项
    if(!LoadConfigFile())return;
    stopFlag_ = false;

    // 创建初始连接的数量
    for(int i = 0; i < initSize_; ++i)
    {
        MySqlConnection *p = new MySqlConnection();
        p->ConnectSQL(IP_, port_, username_, password_, dbname_);
        p->RefreshAliveTime();
        connectionQueue_.push(p);
        connectionCnt_++;
    }

    // 启动一个新的线程，作为生产者线程
    produce_ = std::thread(std::bind(&ConnectionPool::ProduceConnectionTask, this));
    produce_.detach();

    // 启动一个新的定时线程，扫描超过maxIdleTime时间的空闲连接，将其释放掉
    scanner_ = std::thread(std::bind(&ConnectionPool::ScannerConnectionTask, this));
    scanner_.detach();
}

// 停止线程池
void ConnectionPool::Stop() 
{
    // 设置标志位，通知生产者线程和扫描器线程退出
    stopFlag_ = true;

    // 等待生产者线程和扫描器线程安全退出
    if (produce_.joinable()) 
    {
        produce_.join();
    }

    if (scanner_.joinable()) 
    {
        scanner_.join();
    }

    // 清理连接池中的所有连接
    std::unique_lock<std::mutex> lock(queueMutex_);
    while (!connectionQueue_.empty()) 
    {
        MySqlConnection* p = connectionQueue_.front();
        connectionQueue_.pop();
        delete p;  // 删除连接
    }

    connectionCnt_ = 0;
    LOG_INFO << "数据库连接池已清空";
}

// 运行在独立的线程中，专门负责生产新连接
void ConnectionPool::ProduceConnectionTask()
{
    while(!stopFlag_)
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        // 队列不空，此处生产者线程进入等待状态
        while(!connectionQueue_.empty())cv.wait(lock);
        // 连接数量没有到达上限，继续创建新的连接
        if(connectionCnt_ < maxSize_)
        {
            MySqlConnection *p = new MySqlConnection();
            p->ConnectSQL(IP_, port_, username_, password_, dbname_);
            p->RefreshAliveTime();
            connectionQueue_.push(p);
            connectionCnt_++;
        }
        // 通知消费者线程，可以连接
        cv.notify_all();
    }
}

// 扫描超过maxIdleTime的空闲连接，用于连接回收
void ConnectionPool::ScannerConnectionTask()
{
    while(!stopFlag_)
    {
        //模拟定时效果
        std::this_thread::sleep_for(std::chrono::seconds(maxIdleTime_));
        //扫描整个队列释放多余的连接
        std::unique_lock<std::mutex> lock(queueMutex_);
        while(connectionCnt_ > initSize_)
        {
            MySqlConnection *p = connectionQueue_.front();
            if(p->GetAliveTime() >= (maxIdleTime_ * 1000))
            {
                connectionQueue_.pop();
                connectionCnt_--;
                delete p;
            }
            else break; //队首连接没超时，后续的也不会超时
        }
    }
}

// 线程安全的懒汉单例函数接口
ConnectionPool* ConnectionPool::GetConnectionPool()
{
    static ConnectionPool pool;
    return &pool;
}

// 加载配置文件
bool ConnectionPool::LoadConfigFile()
{
    FILE *pf = fopen("../mysql.ini", "r");
    if(pf == nullptr)
    {
        LOG_ERROR << "mysql.ini file is not exist!";
        return false;
    }

    while(!feof(pf))
    {
        char line[1024] = { 0 };
        fgets(line, 1024, pf);
        std::string str = line;
        int idx = str.find('=', 0);
        if(idx == -1) continue;

        int endIdx = str.find("\n", idx);
        std::string key = str.substr(0, idx);
        std::string value = str.substr(idx + 1, endIdx - idx - 1);

        if(key == "ip")IP_ = value;
        else if(key == "port")port_ = atoi(value.c_str()); // 数据库端口号
        else if(key == "username")username_ = value; // 登录用户名
        else if(key == "password")password_ = value; // 密码
        else if(key == "dbname") dbname_ = value; //数据库名
        else if(key == "initSize")initSize_ = atoi(value.c_str()); // 连接池初始连接量
        else if(key == "maxSize")maxSize_ = atoi(value.c_str()); // 连接池最大连接量
        else if(key == "maxIdleTime")maxIdleTime_ = atoi(value.c_str()); // 连接池最大空闲时间
        else if(key == "connectionTimeOut")connectionTimeOut_ = atoi(value.c_str()); //连接池获取连接的超时时间
    }
    return true;
}

// 给外部提供一个接口，从连接池中获取一个可用的空闲连接
std::shared_ptr<MySqlConnection> ConnectionPool::GetConnection()
{
    std::unique_lock<std::mutex> lock(queueMutex_);
    // 等待时间内获得连接
    while(connectionQueue_.empty())
    {
        if(std::cv_status::timeout == cv.wait_for(lock, std::chrono::milliseconds(connectionTimeOut_)))
        {
            if(connectionQueue_.empty())
            {
                LOG_ERROR << "获取连接超时，连接失败！！！";
                return nullptr;
            }
        }
    }

    /*
     * share_ptr智能指针析构时会将connection资源直接delete掉，对应的数据库连接会被关闭，
     * 这里需要重定义share_ptr的资源释放方式，将其归还到连接队列中。
     */
    std::shared_ptr<MySqlConnection> sp(connectionQueue_.front(), [&](MySqlConnection *pcon)
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        pcon->RefreshAliveTime();
        connectionQueue_.push(pcon);
    });
    connectionQueue_.pop();
    // 连接队列为空，通知生产者创建新的连接。或者通知其他消费者消费
    cv.notify_all();

    return sp;
}