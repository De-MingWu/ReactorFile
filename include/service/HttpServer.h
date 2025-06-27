#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <regex>
#include <experimental/filesystem>

#include "TcpServer.h"       // 基于 epoll 的 TCP 服务器封装
#include "ThreadPool.h"      // 简易线程池，用于处理业务请求
#include "HttpResponse.h"    // HTTP 响应生成模块
#include "HttpContext.h"     // HTTP 请求上下文管理
#include "AsyncLog.h"        // 异步日志系统，用于高性能日志输出
#include "LogStream.h"       // 包含 Fmt 和 LogStream 定义
#include "HttpRequest.h"
#include "FileDownContext.h"
#include "FileUploadContext.h"
#include "MySqlConnectionPool.h"

namespace fs = std::experimental::filesystem; 

class HttpServer;

// 定义处理函数类型，指向 HttpServer 类成员函数
using RequestHandler = void (HttpServer::*)(const spConnection&, HttpRequest&, HttpResponse*);

// 路由模式结构
struct RoutePattern 
{
    std::regex pattern;              // 正则表达式模式，用于匹配请求路径
    std::vector<std::string> params; // 路径参数名列表，保存路由中的参数（例如，URL中的:username, :id等）
    RequestHandler handler;          // 请求处理函数，用于处理匹配的请求
    Method method;      // HTTP 方法（如 GET, POST, etc.），用于限制特定 HTTP 方法的匹配

    // 构造函数，初始化路由模式结构体
    RoutePattern(const std::string& patternStr, 
                 const std::vector<std::string>& paramNames,
                 RequestHandler h,
                 Method m)
                : pattern(patternStr),      // 使用正则表达式初始化路由模式
                  params(paramNames),       // 初始化路径参数名列表
                  handler(h),               // 初始化请求处理函数
                  method(m)                // 初始化 HTTP 方法
    {}
};


// 一个支持异步日志记录和线程池处理的简易 HttpServer 类
class HttpServer
{
private:
    TcpServer tcpServer_;    // 基于 Reactor 的 TCP 服务器
    ThreadPool threadPool_;  // 工作线程池，用于并发处理请求
    ConnectionPool *mysqlPool_;

    std::string uploadDir_;             // 上传目录
    std::string mapFile_;               // 文件名映射文件
    std::atomic<int> activeRequests_;   // 活跃请求计数
    std::mutex mapMutex_;               // 保护文件名映射的互斥锁
    std::map<std::string, std::string> fileNameMap_;  // 文件名映射 <服务器文件名, 原始文件名>

    // 路由表
    std::vector<RoutePattern> routes_;

public:
    // 构造函数，初始化服务器监听地址、线程数及线程池大小
    HttpServer(const std::string& ip, 
                       uint16_t port, 
                       int subThreadNum, 
                       int workThreadNum,
                       std::string uploadDir,
                       std::string mapFile);
    ~HttpServer();

    // 启动服务器开始监听与事件循环
    void Start();

    // 停止服务器服务，包括线程池与 TcpServer 以及异步日志
    void StopService();
    

private:
    // 新连接建立时的回调函数
    void HandleNewConnection(spConnection conn);
    // 连接关闭时的回调函数
    void HandleClose(spConnection conn);
    // 连接发生错误时的回调函数
    void HandleError(spConnection conn);
    // 发送数据完成时的回调函数
    void HandleSendComplete(spConnection conn);
    // epoll_wait 超时的回调函数，通常不做日志记录以免膨胀日志文件
    void HandleTimeOut(EventLoop* loop);

    // 生成响应
    std::string GenerateHttpResponse(const std::string& message, 
                                   const HttpStatusCode code);
    // 发送错误报文
    void SendBadRequestResponse(spConnection conn, const HttpStatusCode code, const std::string& message);

    // 有消息到达时的回调函数，如果线程池启用则放入任务队列
    void HandleMessage(spConnection conn, std::string &message);
    // 实际处理 HTTP 请求的函数
    void OnMessage(spConnection conn, std::string &message);

    void OnRequest(const spConnection &conn, HttpRequest &request, HttpResponse* response);
    
    // HTTP 请求处理器，用于处理访问网站首页或静态 HTML 页面
    void HandleIndex(const spConnection &conn, HttpRequest &request, HttpResponse *response);

    // 处理文件上传
    void HandleFileUpload(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 查询数据库并根据文件的所有者和共享信息构建文件列表
    void HandleListFiles(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 处理文件下载请求，根据请求的类型返回不同的文件内容
    void HandleDownload(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 删除文件
    void HandleDelete(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 文件分享处理函数
    void HandleShareFile(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 通过分享码访问文件
    void HandleShareAccess(const spConnection &conn, HttpRequest &request, HttpResponse *response); 
    // 处理通过分享链接下载文件（支持权限判断与断点续传）
    void HandleShareDownload(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 处理获取分享信息请求（校验分享码、提取码，并返回文件元信息）
    void HandleShareInfo(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 处理 favicon.ico 请求（返回浏览器标签页图标）
    void HandleFavicon(const spConnection &conn, HttpRequest &request, HttpResponse *response);

    // 用户注册
    void HandleRegister(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 用户登录
    void HandleLogin(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 退出
    void HandleLogout(const spConnection &conn, HttpRequest &request, HttpResponse *response);
    // 检索用户
    void HandleSearchUsers(const spConnection &conn, HttpRequest &request, HttpResponse *response);

    // 加载文件名映射
    void LoadFileNameMap(); 
    // 内部函数，不加锁，加载文件名映射
    void LoadFileNameMapInternal();
    // 保存文件名映射
    void SaveFileNameMap(); 
    void SaveFileNameMapInternal(); 

    // 初始化路由表
    void InitRoutes();
    // 添加精确匹配的路由
    void AddRoute(const std::string& path, Method method, RequestHandler handler);
    // 添加带参数的路由
    void AddRoute(const std::string& pattern, Method method, 
             RequestHandler handler, const std::vector<std::string>& paramNames); 

    
    // 保存会话
    void SaveSession(const std::string& sessionId, int userId, const std::string& username);
    // 验证会话，检查 sessionId 是否有效
    bool ValidateSession(const std::string& sessionId, int& userId, std::string& username);
    // 结束会话，删除 session 数据
    void DeleteSession(const std::string& sessionId);

};

inline std::string MethodToString(Method method) 
{
    switch (method) 
    {
        case Method::kGet: return "GET";
        case Method::kPost: return "POST";
        case Method::kHead: return "HEAD";
        case Method::kPut: return "PUT";
        case Method::kDelete: return "DELETE";
        default: return "INVALID";
    }
}
#endif // HTTPSERVER_H
