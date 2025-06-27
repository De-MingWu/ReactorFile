#include <sstream> 
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>

#include "HttpServer.h"
#include "Log.h"  // 引入 Log 头文件
#include "Public.h"

using json = nlohmann::json; 

// 构造函数，初始化 TcpServer、线程池及日志
HttpServer::HttpServer(const std::string& ip, 
                       uint16_t port, 
                       int subThreadNum, 
                       int workThreadNum, 
                       std::string uploadDir,
                       std::string mapFile)
           :tcpServer_(ip, port, subThreadNum),
            threadPool_(workThreadNum, "HttpWorks"),
            uploadDir_(uploadDir),
            mapFile_(mapFile)
{
    // 设置 TcpServer 各种事件回调绑定，使用 std::bind 绑定成员函数及 this 指针
    tcpServer_.SetNewConnectionCB(std::bind(&HttpServer::HandleNewConnection, this, std::placeholders::_1));
    tcpServer_.SetCloseConnectionCB(std::bind(&HttpServer::HandleClose, this, std::placeholders::_1));
    tcpServer_.SetErrorConnectionCB(std::bind(&HttpServer::HandleError, this, std::placeholders::_1));
    tcpServer_.SetHandleMessageCB(std::bind(&HttpServer::HandleMessage, this, std::placeholders::_1, std::placeholders::_2));
    //tcpServer_.SetSendCompleteCB(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
    tcpServer_.SetTimeOutCB(std::bind(&HttpServer::HandleTimeOut, this, std::placeholders::_1));

    // 初始化操作
    // 检查并创建目录
    if (!fs::exists(uploadDir_)) 
    {
        fs::create_directory(uploadDir_);
    }

    //数据库连接池初始化
    mysqlPool_ = ConnectionPool::GetConnectionPool();

    // 加载文件名映射
    LoadFileNameMap();
        
    // 初始化路由表
    InitRoutes();
}

HttpServer::~HttpServer() 
{
    // 析构时停止服务
    StopService();
}

// 启动 TCP 服务器
void HttpServer::Start() 
{
    tcpServer_.Start();
}

// 停止服务，停止线程池及日志，关闭 TCP 服务
void HttpServer::StopService() 
{
    threadPool_.StopThread();
    // 保存映射文件
    SaveFileNameMap();
    // 使用 Log 输出日志
    LOG_INFO << "HttpServer: 工作线程已停止。";

    // 清理数据库连接池
    mysqlPool_->Stop();  // 清空连接池中的所有数据库连接
    LOG_INFO << "HttpServer: 数据库连接池已清空。";

    tcpServer_.StopService();
    LOG_INFO << "HttpServer: TCPservice stop。";
}

// 新连接回调，创建 HttpContext 上下文对象用于请求解析等
void HttpServer::HandleNewConnection(spConnection conn) 
{
    // 为每个新连接创建一个 HttpContext
    conn->SetContext(std::make_shared<HttpContext>());
    
    // 使用 Log 输出日志
    LOG_INFO << "HttpServer: 新连接建立(fd=" << conn->GetFd()
             << ", ip=" << conn->GetIP() << ", port=" << conn->GetPort() << ")";
}

// 连接关闭回调，清理上下文信息
void HttpServer::HandleClose(spConnection conn) 
{
    if (auto context = std::static_pointer_cast<HttpContext>(conn->GetContext())) 
    {
        if (auto uploadContext = context->GetContext<FileUploadContext>()) 
        {
            LOG_INFO << "Cleaning up upload context for file: " << uploadContext->GetFileName();
        }
    }
    conn->SetContext(std::shared_ptr<void>());
    conn->HttpClose();
    // 使用 Log 输出日志
    LOG_INFO << "HttpServer: 连接关闭 (IP: " << conn->GetIP() << ")";
}

// 连接错误回调，记录日志
void HttpServer::HandleError(spConnection conn) 
{

}

// 数据发送完成回调
void HttpServer::HandleSendComplete(spConnection conn) 
{
    // 使用 Log 输出日志
    LOG_INFO << "HttpServer: 数据发送完毕 fd=" << conn->GetFd();
}

// 超时处理回调
void HttpServer::HandleTimeOut(EventLoop* loop) 
{
    // 可扩展逻辑
}

// 根据 HttpRequest 生成 HttpResponse 并返回完整响应字符串
std::string HttpServer::GenerateHttpResponse(const std::string& message, 
                                             const HttpStatusCode code)
{
    json responses = {
            {"code", static_cast<int>(code)},
            {"message", message}
        };
    HttpResponse response(true);  
    response.SetStatusCode(code);
    response.SetContentType("application/json");
    response.AddHeader("Connection", "close");
    response.SetBody(responses.dump());

    return response.ResponseMessage();
}

// 发送 400 Bad Request 响应
void HttpServer::SendBadRequestResponse(spConnection conn, const HttpStatusCode code, const std::string& message) 
{
    HttpResponse response(true);

    json body = {
            {"code", static_cast<int>(code)},
            {"message", message}
        };
    response.SetStatusCode(code);
    response.SetContentType("application/json");
    response.AddHeader("Connection", "close");
    response.SetBody(body.dump());

    const std::string& resp = response.ResponseMessage();
    conn->SendData(resp.data(), resp.size());

    // 使用 Log 输出日志
    LOG_ERROR << "HttpServer: 请求解析失败，返回 400";
}

// 处理客户端的请求报文
void HttpServer::HandleMessage(spConnection conn, std::string &message)
{
    if(threadPool_.GetSize() == 0)
    {
        //没有工作线程， 直接在I/O线程中计算
        OnMessage(conn, message);
    }
    else
        threadPool_.AddTasks(std::bind(&HttpServer::OnMessage, this, conn, message));
}

// 消息处理回调
void HttpServer::OnMessage(spConnection conn, std::string &message)
{
    auto ctx = std::static_pointer_cast<HttpContext>(conn->GetContext());
    if (!ctx) 
    {
        LOG_ERROR << "HttpContext is null";
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "内部错误");
        return;
    }

    // 多次解析：每次传入当前 buffer
    HttpRequestParseState state = ctx->ParseRequest(message.data(), static_cast<int>(message.size()));
    HttpRequest* request = ctx->GetRequest();

    // 判断解析结果状态
    switch (state)
    {
        case HttpRequestParseState::kINVALID:
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "请求解析失败");
            return;

        case HttpRequestParseState::kHeadersComplete:
            if (request->GetMethod() == Method::kPost && request->GetUrl() == "/upload") 
            {
                HttpResponse response(false);  // 不关闭连接
                OnRequest(conn, *request, &response);

                return;  // 暂不 reset 上下文，后续还要继续处理 body
            }
            break;

        case HttpRequestParseState::COMPLETE:
        {
            HttpResponse response(false);
            OnRequest(conn, *request, &response);
        
            ctx->ResetContextStatus();  // 仅在请求完整处理后重置
            break;
        }

        default:
            LOG_INFO << "等待更多数据，当前状态: " << static_cast<int>(state);
            break;
    }
}

void HttpServer::OnRequest(const spConnection &conn, HttpRequest &request, HttpResponse *response) 
{
    std::string path = request.GetUrl();
    LOG_INFO << "Headers " + request.GetMethodString() + " " + path;
    LOG_INFO << "Content-Type: " + request.GetHeader("Content-Type");
    LOG_INFO << "Body size: " + std::to_string(request.GetBody().size());

    try {
        // 查找匹配的路由
        for (const auto& route : routes_) 
        {
            // 检查请求方法是否匹配
            if (route.method != request.GetMethod()) 
            {
                LOG_INFO << "Method mismatch: expected " + MethodToString(route.method) + ", got " + request.GetMethodString();
                continue;
            }

            // 路径匹配
            std::smatch matches;
            if (std::regex_match(path, matches, route.pattern)) 
            {
                LOG_INFO << "Found matching route: " + path;

                // 提取路径参数，将参数存储到请求对象中
                for (size_t i = 0; i < route.params.size() && i + 1 < matches.size(); ++i) 
                {
                    request.SetRequestParams(route.params[i], matches[i + 1]);
                    std::cout << route.params[i] << ": " << matches[i + 1] << std::endl;
                }
                // 调用处理函数
                (this->*route.handler)(conn, request, response);
                return;
            }
        }

        // 未找到匹配的路由，返回404
        LOG_WARN << "No matching route found for " + path;
        SendBadRequestResponse(conn, HttpStatusCode::k404NotFound, "Not Found");
    } 
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Error processing request: " + std::string(e.what());
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Internal Server Error");
    }
}

// HTTP 请求处理器，用于处理访问网站首页或静态 HTML 页面
void HttpServer::HandleIndex(const spConnection &conn, HttpRequest &request, HttpResponse *response) 
{
    // 设置 HTTP 响应头：200 OK、Content-Type 为 HTML
    response->SetStatusCode(HttpStatusCode::k200OK);
    response->SetStatusMessage("OK");
    response->SetContentType("text/html; charset=utf-8");

    // 获取请求路径
    std::string path = request.GetUrl();
    std::string filePath;

    LOG_INFO << "path = " << path;

    // 获取当前文件所在目录作为项目根路径（编译期绝对路径）
    std::string currentDir = __FILE__;
    std::string::size_type pos = currentDir.find_last_of("/");
    std::string projectRoot = currentDir.substr(0, pos);  // 去除文件名，仅保留目录
    LOG_INFO << "projectRoot = " << projectRoot;

    // 根据请求路径选择 HTML 文件
    if (path == "/register.html") 
    {
        filePath = projectRoot + "/register.html";
    } 
    else if (path == "/share.html" || path.find("/share/") == 0) 
    {
        filePath = projectRoot + "/share.html";
    } 
    else 
    {
        filePath = projectRoot + "/index.html";
    }

    // 打开 HTML 文件
    std::ifstream file(filePath.c_str());  // C++11 中显式使用 .c_str()
    if (!file.is_open()) 
    {
        LOG_ERROR << "Failed to open " << filePath;
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Failed to open " + filePath);
        return;
    }

    // 读取整个 HTML 文件内容到字符串中
    std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // 设置响应体和头部
    response->AddHeader("Connection", "close");
    response->SetBody(html);

    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());
    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}

/**
 * 处理 multipart/form-data 文件上传请求的完整逻辑
 * 1.提取 X-Session-ID 头部并验证，确保用户已登录
 * 2.初始化上传上下文（首次请求）
 * 3.继续写入文件块（后续请求）
 * 4.判断是否上传完成
 */
void HttpServer::HandleFileUpload(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    // 1. 验证会话，提取 session ID 并校验
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");
    int userId;
    std::string usernameFromSession;
    
    if (!ValidateSession(sessionId, userId, usernameFromSession)) 
    {
        LOG_ERROR << "HandleFileUpload Sessionid is null";
        SendBadRequestResponse(conn, HttpStatusCode::k401Unauthorized, "未登录或会话已过期");
        return;
    }

    // 2. 获取连接上下文（HttpContext）并确保类型有效
    auto httpContext = std::static_pointer_cast<HttpContext>(conn->GetContext());
    if (!httpContext) 
    {
        LOG_ERROR << "HttpContext is null";
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Internal Server Error");
        return;
    }

    LOG_INFO << "body.size() = " << request.GetBody().size();

    // 3. 尝试获取已有上传上下文，判断是首次请求还是续传请求
    std::shared_ptr<FileUploadContext> uploadContext = httpContext->GetContext<FileUploadContext>();

    // 3.1. 首次上传请求（尚未创建上传上下文）
    if (!uploadContext) 
    {
        // 提取 Content-Type 并从中获取 multipart 边界
        std::string contentType = request.GetHeader("Content-Type");
        if (contentType.empty()) 
        {
            LOG_ERROR << "HandleFileUpload contentType is null";
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Content-Type header is missing");
            return;
        }
        //正则匹配 boundary，获取分界符
        std::regex boundaryRegex("boundary=(.+)$");
        std::smatch matches;
        if (!std::regex_search(contentType, matches, boundaryRegex)) 
        {
            LOG_ERROR << "HandleFileUpload boundaryRegex is Error";
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Invalid Content-Type");
            return;
        }
        std::string boundary = "--" + matches[1].str();
        LOG_INFO << "Boundary: " << boundary;

        try 
        {
            // 解析原始文件名
            std::string originalFilename;

            // 优先使用 X-File-Name 头部
            std::string headerFilename = request.GetHeader("X-File-Name");
            
            if (!headerFilename.empty()) 
            {
                originalFilename = URLDecode(headerFilename);
                LOG_INFO << "Got filename from X-File-Name header: " << originalFilename;
            } 
            else 
            {
                // 否则尝试从请求体中解析 Content-Disposition 的文件名
                std::string body = request.GetBody();
                if (body.empty()) 
                {
                    LOG_ERROR << "HandleFileUpload body is null";
                    SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Request body is empty");
                    return;
                }
                std::regex filenameRegex("Content-Disposition:.*filename=\"([^\"]+)\"");
                if (std::regex_search(body, matches, filenameRegex) && matches[1].matched) 
                {
                    originalFilename = matches[1].str();
                    LOG_INFO << "Got filename from Content-Disposition: " << originalFilename;
                } 
                else 
                {
                    originalFilename = "unknown_file";
                    LOG_INFO << "Using default filename: " << originalFilename;
                }
            }

            // 生成唯一服务器端文件名，并创建上传上下文对象
            std::string filename = GenerateUniqueFileName("upload");
            std::string filepath = uploadDir_ + "/" + filename;
            uploadContext = std::make_shared<FileUploadContext>(filepath, originalFilename);
            httpContext->SetContext(uploadContext);  // 绑定到连接上下文中
            uploadContext->SetBoundary(boundary);     // 设置 multipart 边界

            // 提取正文开始位置
            std::string body = request.GetBody();
            size_t pos = body.find("\r\n\r\n");
            if (pos != std::string::npos) 
            {
                pos += 4; // 跳过头部换行符

                std::string endBoundary = boundary + "--";
                size_t endPos = body.find(endBoundary);
                if (endPos != std::string::npos) 
                {
                    // 如果存在结束边界，写入文件体内容并标记上传完成
                    if (endPos > pos) 
                    {
                        uploadContext->WriteData(body.data() + pos, endPos - pos);
                        LOG_INFO << "Wrote " << endPos - pos << " bytes before end boundary, total: " << uploadContext->GetTotalBytes();
                    }
                    uploadContext->SetState(State::kComplete);
                } 
                else 
                {
                    // 否则为后续分块传输，继续等待边界
                    uploadContext->WriteData(body.data() + pos, body.size() - pos);
                    LOG_INFO << "Wrote " << body.size() - pos << " bytes, total: " << uploadContext->GetTotalBytes();
                    uploadContext->SetState(State::kExpectBoundary);
                }
            }
            request.SetBody("");  // 清空 body，释放内存
            LOG_INFO << "Created upload context for file: " << filepath;
        } 
        catch (const std::exception& e) 
        {
            LOG_ERROR << "Failed to create upload context: " << e.what();
            SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Failed to create file");
            return;
        }

    } 
    else 
    {
        // 3.2. 后续请求（上传中的文件块）
        try 
        {
            std::string body = request.GetBody();
            if (!body.empty()) 
            {
                LOG_INFO << "uploadContext->GetState() = " << static_cast<int>(uploadContext->GetState());

                switch (uploadContext->GetState()) 
                {
                    // 等待下一个文件块
                    case State::kExpectBoundary: 
                    {
                        std::string endBoundary = uploadContext->GetBoundary() + "--";
                        size_t endPos = body.find(endBoundary);
                        if (endPos != std::string::npos) 
                        {
                            // 写入内容并标记为完成
                            if (endPos > 0) 
                            {
                                uploadContext->WriteData(body.data(), endPos);
                                LOG_INFO << "Wrote " << endPos << " bytes before end boundary, total: " << uploadContext->GetTotalBytes();
                            }
                            uploadContext->SetState(State::kComplete);
                            break;
                        }

                        // 检查普通边界
                        size_t boundaryPos = body.find(uploadContext->GetBoundary());
                        if (boundaryPos != std::string::npos) 
                        {
                            size_t contentStart = body.find("\r\n\r\n", boundaryPos);
                            if (contentStart != std::string::npos) 
                            {
                                contentStart += 4;
                                if (boundaryPos > 0) 
                                {
                                    uploadContext->WriteData(body.data(), boundaryPos);
                                    LOG_INFO << "Wrote " << boundaryPos << " bytes, total: " << uploadContext->GetTotalBytes();
                                }
                                uploadContext->SetState(State::kExpectContent);
                            }
                        } 
                        else 
                        {
                            // 继续写入当前块
                            uploadContext->WriteData(body.data(), body.size());
                            LOG_INFO << "Wrote " << body.size() << " bytes, total: " << uploadContext->GetTotalBytes();
                        }
                        break;
                    }
                    // 文件上传
                    case State::kExpectContent: 
                    {
                        size_t boundaryPos = body.find(uploadContext->GetBoundary());
                        if (boundaryPos != std::string::npos) 
                        {
                            uploadContext->WriteData(body.data(), boundaryPos);
                            LOG_INFO << "Wrote " << boundaryPos << " bytes, total: " << uploadContext->GetTotalBytes();
                            uploadContext->SetState(State::kExpectBoundary);
                        } 
                        else 
                        {
                            uploadContext->WriteData(body.data(), body.size());
                            LOG_INFO << "Wrote " << body.size() << " bytes, total: " << uploadContext->GetTotalBytes();
                        }
                        break;
                    }
                    // 上传完毕
                    case State::kComplete:
                        // 忽略后续数据
                        break;

                    default:
                        LOG_INFO << "Unknown state: " << static_cast<int>(uploadContext->GetState());
                        break;
                }
            }
        } 
        catch (const std::exception& e) 
        {
            LOG_ERROR << "Error processing data chunk: " << e.what();
            SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Failed to process data");
            return;
        }
    }

    request.SetBody("");  // 清空请求体，节省内存

    // 4. 判断是否已上传完成（终态或客户端标记 EOF）
    if (uploadContext->GetState() == State::kComplete || httpContext->GetCompleteRequest()) 
    {
        // std::string serverFileName = fs::path(uploadContext->GetFilename()).filename().string();
        std::string serverFileName = uploadContext->GetFileName();

        // 查找最后一个路径分隔符的位置
        size_t pos = serverFileName.find_last_of("/\\");  // 适配 Linux 和 Windows 路径分隔符
        if (pos != std::string::npos) 
        {
            serverFileName = serverFileName.substr(pos + 1);  // 提取文件名部分
        }
        
        std::string originalFileName = uploadContext->GetOriginalFileName();
        uintmax_t fileSize = uploadContext->GetTotalBytes();

        std::string fileType = GetFileType(originalFileName);

        std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
        MYSQL* mysql = mysqlConn->GetRawConnection();

        // 写入数据库记录
        std::string query = "INSERT INTO files (fileName, original_FileName, file_size, file_type, user_id) VALUES ('" +
            EscapeString(serverFileName, mysql) + "', '" +
            EscapeString(originalFileName, mysql) + "', " +
            std::to_string(fileSize) + ", '" +
            EscapeString(fileType, mysql) + "', " +
            std::to_string(userId) + ")";

        int fileId = mysqlConn->Update(query);
        LOG_INFO << "文件：" << originalFileName << "记录写入数据库";

        // 构造 JSON 响应
        json jsonStr = 
        {
            {"code", 0},
            {"message", "上传成功"},
            {"fileId", fileId},
            {"FileName", serverFileName},
            {"originalFileName", originalFileName},
            {"size", fileSize}
        };

        // 设置响应头与 body
        response->SetStatusCode(HttpStatusCode::k200OK);
        response->SetStatusMessage("OK");
        response->SetContentType("application/json");
        response->AddHeader("Connection", "close");
        response->SetBody(jsonStr.dump());

        const std::string& resp = response->ResponseMessage();
        conn->SendData(resp.data(), resp.size());
        httpContext->SetContext(nullptr);
        conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleClose, this, std::placeholders::_1));
    } 
    else 
    {
        // 数据未传输完，等待下一个请求
        LOG_INFO << "Waiting for more data, current state: " 
                 << static_cast<int>(uploadContext->GetState());
    }
}

// 查询数据库并根据文件的所有者和共享信息构建文件列表
void HttpServer::HandleListFiles(const spConnection &conn, HttpRequest &request, HttpResponse *response) 
{
    // 1. 验证会话，检查用户是否已登录或会话是否过期
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");

    int userId;
    std::string usernameFromSession;
    
    if (!ValidateSession(sessionId, userId, usernameFromSession)) 
    {
        LOG_ERROR << "HandleListFiles session is null";
        SendBadRequestResponse(conn, HttpStatusCode::k401Unauthorized, "未登录或会话已过期");
        return;  // 返回 401 错误并终止
    }

    // 2. 获取请求中传递的文件列表类型（默认为 "my"）
    std::string listType = request.GetHeader("type");  // "my", "shared", "all"
    if(listType.empty()) listType = "my";
    
    std::string query;
    if (listType == "my") 
    {
        // 2.1 获取当前用户的文件列表
        query = "SELECT f.id, f.filename, f.original_filename, f.file_size, f.file_type, "
                "f.created_at, 1 as is_owner FROM files f WHERE f.user_id = " + std::to_string(userId);
    } 
    else if (listType == "shared") 
    {
        // 2.2 获取共享给当前用户的文件
        query = "SELECT f.id, f.filename, f.original_filename, f.file_size, f.file_type, "
                "f.created_at, 0 as is_owner FROM files f "
                "JOIN file_shares fs ON f.id = fs.file_id "
                "WHERE (fs.shared_with_id = " + std::to_string(userId) + " OR fs.share_type = 'public') "
                "AND f.user_id != " + std::to_string(userId);
    } 
    else if (listType == "all") 
    {
        // 2.3 获取当前用户的文件以及共享给当前用户的文件
        query = "SELECT f.id, f.filename, f.original_filename, f.file_size, f.file_type, "
                "f.created_at, CASE WHEN f.user_id = " + std::to_string(userId) + " THEN 1 ELSE 0 END as is_owner "
                "FROM files f "
                "LEFT JOIN file_shares fs ON f.id = fs.file_id "
                "WHERE f.user_id = " + std::to_string(userId) + " OR "
                "fs.shared_with_id = " + std::to_string(userId) + " OR fs.share_type = 'public'";
    }
    
    LOG_INFO << "query = " << query;  // 打印生成的 SQL 查询
    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    MYSQL_RES* result = mysqlConn->Query(query);  // 执行查询并获取结果

    json jsonStr;
    jsonStr["code"] = 0;
    jsonStr["message"] = "Success";
    json files = json::array();  // 存储文件信息的 JSON 数组

    // 3. 处理查询结果
    if (result) 
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) 
        {
            // 解析每一行文件信息
            int fileId = std::stoi(row[0]);
            std::string fileName = row[1];
            std::string originalFileName = row[2];
            uintmax_t fileSize = std::stoull(row[3]);
            std::string fileType = row[4];
            std::string createdAt = row[5];
            bool isOwner = (std::stoi(row[6]) == 1);  // 判断当前用户是否是文件所有者

            // 4. 如果文件是当前用户的，获取分享信息
            json shareInfo = nullptr;
            if (isOwner) 
            {
                // 4.1 查询文件的分享信息
                std::string shareQuery = "SELECT share_type, shared_with_id, share_code, expire_time, extract_code FROM file_shares "
                                          "WHERE file_id = " + std::to_string(fileId);
                MYSQL_RES* shareResult = mysqlConn->Query(shareQuery);
                
                if (shareResult && mysql_num_rows(shareResult) > 0) 
                {
                    // 获取下一行的数据，并返回该行数据的一个指针
                    MYSQL_ROW shareRow = mysql_fetch_row(shareResult);
                    std::string shareType = shareRow[0];

                    // 4.2 构建分享信息
                    shareInfo = 
                    {
                        {"type", shareType},
                        {"shareCode", shareRow[2] ? shareRow[2] : ""}  // 如果有 shareCode，则填充
                    };

                    // 如果是受保护分享，包含提取码
                    if (shareType == "protected" && shareRow[4]) 
                    {
                        shareInfo["extractCode"] = shareRow[4];
                    }

                    // 如果是用户分享，获取共享给的用户的用户名
                    if (shareType == "user" && shareRow[1]) 
                    {
                        int sharedWithId = std::stoi(shareRow[1]);
                        std::string userQuery = "SELECT username FROM users WHERE id = " + std::to_string(sharedWithId);
                        MYSQL_RES* userResult = mysqlConn->Query(userQuery);
                        if (userResult && mysql_num_rows(userResult) > 0) 
                        {
                            MYSQL_ROW userRow = mysql_fetch_row(userResult);
                            shareInfo["sharedWithUsername"] = userRow[0];
                            shareInfo["sharedWithId"] = sharedWithId;
                        }
                        if (userResult) mysql_free_result(userResult);  // 释放用户查询结果
                    }

                    // 如果有过期时间，填充
                    if (shareRow[3]) 
                    {
                        shareInfo["expireTime"] = shareRow[3];
                    }
                }

                if (shareResult) mysql_free_result(shareResult);  // 释放分享信息查询结果
            }

            // 5. 构建文件信息 JSON 对象
            json fileInfo = {
                {"id", fileId},
                {"name", fileName},
                {"originalName", originalFileName},
                {"size", fileSize},
                {"type", fileType},
                {"createdAt", createdAt},
                {"isOwner", isOwner}
            };

            // 6. 如果存在分享信息，添加到文件信息中
            if (shareInfo != nullptr) 
            {
                fileInfo["shareInfo"] = shareInfo;
            }

            files.push_back(fileInfo);  // 将文件信息添加到文件数组中
        }
        jsonStr["files"] = files;  // 添加文件列表到响应中
    }

    // 7. 设置 HTTP 响应头部和内容
    response->SetStatusCode(HttpStatusCode::k200OK);
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->AddHeader("Connection", "close");
    response->SetBody(jsonStr.dump());  // 将文件信息转为 JSON 格式并设置到响应体中

    // 8. 设置写完成回调，关闭连接
    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());
    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}

// 处理文件下载请求，根据请求的类型返回不同的文件内容
void HttpServer::HandleDownload(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    response->SetAsync(true);
    // 1. 获取请求的文件名
    std::string fileName = request.GetRequestParamsByKey("filename");
    if (fileName.empty()) 
    {
        LOG_ERROR << "HandleDownload fileName is null";
        SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Missing fileName");
        return;  // 如果文件名为空，返回 400 错误
    }
    
    // 2. 获取会话 ID
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");
    if (sessionId.empty()) 
    {
        sessionId = request.GetRequestParamsByKey("sessionId");  // 尝试从 URL 查询参数获取 sessionId
        LOG_INFO << "从URL查询参数获取sessionId: " << sessionId;
    }
    
    int userId = 0;
    std::string usernameFromSession;
    bool isAuthenticated = ValidateSession(sessionId, userId, usernameFromSession);  // 验证用户是否已登录
    
    // 获取分享码和提取码（如果有）
    std::string shareCode = request.GetRequestParamsByKey("code");
    std::string extractCode = request.GetRequestParamsByKey("extract_code");
    LOG_INFO << "shareCode = " << shareCode << ", extractCode = " << extractCode;
    
    // 3. 构造查询 SQL，获取文件信息和权限
    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    MYSQL* mysql = mysqlConn->GetRawConnection();
    std::string query;
    if (!shareCode.empty()) 
    {
        // 通过分享链接访问
        query = "SELECT f.id, f.filename, f.original_filename, f.user_id, "
                "fs.share_type, fs.shared_with_id, fs.extract_code "
                "FROM files f "
                "JOIN file_shares fs ON f.id = fs.file_id "
                "WHERE f.filename = '" + EscapeString(fileName, mysql) + "' "
                "AND fs.share_code = '" + EscapeString(shareCode, mysql) + "' "
                "AND (fs.expire_time IS NULL OR fs.expire_time > NOW())";
    } 
    else 
    {
        // 直接访问，验证会话
        if (!isAuthenticated) 
        {
            LOG_ERROR << "HandleDownload session is null";
            SendBadRequestResponse(conn, HttpStatusCode::k401Unauthorized, "请先登录");
            return;  // 如果未认证，返回 401 错误
        }
        query = "SELECT f.id, f.filename, f.original_filename, f.user_id, "
                "NULL as share_type, NULL as shared_with_id, NULL as extract_code "
                "FROM files f WHERE f.filename = '" + EscapeString(fileName, mysql) + "'";
    }
    
    LOG_INFO << "查询文件信息: " << query;
    MYSQL_RES* result = mysqlConn->Query(query);
    
    // 4. 检查文件是否存在
    if (!result || mysql_num_rows(result) == 0) 
    {
        if (result) mysql_free_result(result);
        LOG_ERROR << "HandleDownload File not found";
        SendBadRequestResponse(conn, HttpStatusCode::k404NotFound, "File not found");
        return;  // 文件未找到，返回 404 错误
    }
    
    // 5. 获取文件信息
    MYSQL_ROW row = mysql_fetch_row(result);
    int fileId = std::stoi(row[0]);
    std::string serverfileName = row[1];
    std::string originalfileName = row[2];
    int fileOwnerId = std::stoi(row[3]);
    std::string shareType = row[4] ? row[4] : "";
    int sharedWithId = row[5] ? std::stoi(row[5]) : 0;
    std::string dbExtractCode = row[6] ? row[6] : "";
    mysql_free_result(result);
    
    // 6. 检查访问权限
    bool hasPermission = false;
    
    // 文件所有者始终有权限
    if (isAuthenticated && userId == fileOwnerId) 
    {
        hasPermission = true;
    } 
    else if (!shareCode.empty()) 
    {
        // 分享文件的访问权限检查
        if (shareType == "public") 
        {
            hasPermission = true;  // 公共文件，任何人均可访问
        } 
        else if (shareType == "protected" && extractCode == dbExtractCode) 
        {
            hasPermission = true;  // 需要提取码的文件，验证提取码
        } 
        else if (shareType == "user" && userId == sharedWithId) 
        {
            hasPermission = true;  // 用户分享文件，验证共享用户
        }
    } 
    else if (isAuthenticated) 
    {
        // 非分享文件，用户必须是文件所有者
        hasPermission = (userId == fileOwnerId);
    }
    
    if (!hasPermission) 
    {
        if (shareType == "protected" && (extractCode.empty() || extractCode != dbExtractCode)) 
        {
            LOG_ERROR << "提取码错误或未提供";
            SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "需要正确的提取码");
        } 
        else 
        {
            LOG_ERROR << "权限检查失败 - 用户ID: " << userId << ", 文件ID: " << fileId;
            SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "您没有权限访问此文件");
        }
        return;  // 权限不足，返回 403 错误
    }
    
    LOG_INFO << "权限检查通过，准备下载文件";
    std::string filepath = uploadDir_ + "/" + serverfileName;
    
    try 
    {
        // 7. 检查文件是否存在
        if (!fs::exists(filepath) || !fs::is_regular_file(filepath)) 
        {
            LOG_ERROR << "HandleListFiles session is null";
            SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "File not found");
            return;  // 文件不存在，返回 404 错误
        }
        
        uintmax_t fileSize = fs::file_size(filepath);  // 获取文件大小
        
        // 8. 处理 HEAD 请求，只返回文件信息，不下载文件
        if (request.GetMethod() == Method::kHead) 
        {
            response->SetStatusCode(HttpStatusCode::k200OK);
            response->SetStatusMessage("OK");
            response->SetContentType("application/octet-stream");
            response->AddHeader("Content-Length", std::to_string(fileSize));
            response->AddHeader("Accept-Ranges", "bytes");
            response->AddHeader("Connection", "close");

            const std::string& resp = response->ResponseMessage();
            conn->SendData(resp.data(), resp.size());
            return;  // 完成文件信息返回
        }
        
        // 9. 处理 Range 请求，支持部分下载
        std::string rangeHeader = request.GetHeader("Range");
        uintmax_t startPos = 0;
        uintmax_t endPos = fileSize - 1;
        bool isRangeRequest = false;
        
        if (!rangeHeader.empty()) 
        {
            // 解析 Range 头部
            std::regex rangeRegex("bytes=(\\d+)-(\\d*)");
            std::smatch matches;
            if (std::regex_search(rangeHeader, matches, rangeRegex)) 
            {
                startPos = std::stoull(matches[1]);
                if (!matches[2].str().empty()) 
                {
                    endPos = std::stoull(matches[2]);
                }
                isRangeRequest = true;
                
                // 验证范围
                if (startPos >= fileSize) 
                {
                    LOG_ERROR << "HandleDownload Range Not Satisfiable";
                    SendBadRequestResponse(conn, HttpStatusCode::k416RangeNotSatisfiable, "Range Not Satisfiable");
                    return;
                }
                
                if (endPos >= fileSize) 
                {
                    endPos = fileSize - 1;  // 调整结束位置
                }
            }
        }
        
        LOG_INFO << "startPos: " << startPos << ", endPos: " << endPos;
        
        // 10. 获取 HttpContext
        auto httpContext = std::static_pointer_cast<HttpContext>(conn->GetContext());
        if (!httpContext) 
        {
            LOG_ERROR << "HandleDownload HttpContext is null";
            SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Internal Server Error");
            return;
        }
        
        // 11. 获取下载上下文，如果没有，则创建新的
        std::shared_ptr<FileDownContext> downContext = httpContext->GetContext<FileDownContext>();
        
        if (!downContext)  
        {
            // 11.1 首次请求此文件，创建新的下载上下文，保存文件路径和原始文件名
            downContext = std::make_shared<FileDownContext>(filepath, originalfileName);

            // 11.2 将下载上下文保存在连接的 HttpContext 中
            httpContext->SetContext(downContext);

            // 11.3 如果是 Range 请求，设置响应码为 206 Partial Content，并添加 Content-Range 头
            if (isRangeRequest) 
            {
                response->SetStatusCode(HttpStatusCode::k206PartialContent);  // 设置状态码为部分内容
                response->SetStatusMessage("Partial Content");                // 设置状态消息
                response->AddHeader("Content-Range", 
                    "bytes " + std::to_string(startPos) + "-" + 
                    std::to_string(endPos) + "/" + std::to_string(fileSize)); // 指明返回的字节范围
            } 
            else 
            {
                // 否则为普通完整下载，返回 200 OK
                response->SetStatusCode(HttpStatusCode::k200OK);
                response->SetStatusMessage("OK");
                response->AddHeader("Content-Length", std::to_string(endPos - startPos + 1));
            }

            // 11.4 设置返回类型为二进制流，表示文件下载
            response->SetContentType("application/octet-stream");

            // 11.5 设置 Content-Disposition，提示浏览器以附件形式下载，并指定文件名
            response->AddHeader("Content-Disposition", 
                "attachment; fileName=\"" + originalfileName + "\"");
            response->AddHeader("Transfer-Encoding", "chunked");  // 关键设置
            
            response->AddHeader("Accept-Ranges", "bytes");
            // 发送消息头部
            const std::string& resp = response->ResponseMessage();
            //std::cout << resp << std::endl;
            conn->SendData(resp.data(), resp.size());
            downContext->SeekTo(startPos);
        }
        conn->SetSendCompleteCallback([this, downContext](spConnection c) 
        {
            std::string chunk;
            if (downContext->ReadNextChunk(chunk)) {
                // 格式：<chunk-size>\r\n<chunk-data>\r\n
                std::ostringstream oss;
                oss << std::hex << chunk.size() << "\r\n" << chunk << "\r\n";
                std::string chunkedData = oss.str();
                c->SendData(chunkedData.data(), chunkedData.size());
            } 
            else 
            {
                // 结尾块：0\r\n\r\n，表示发送完成
                c->SendData("0\r\n\r\n", 5);
                c->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
            }
        });
        
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Error during file download: " << e.what();
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Download failed");
        return;  // 发生异常时返回 500 错误
    }
}

// 删除文件
void HttpServer::HandleDelete(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    // 1. 验证用户会话是否合法
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");
    int userId;
    std::string usernameFromSession;

    if (!ValidateSession(sessionId, userId, usernameFromSession)) 
    {
        LOG_WARN << "HandleDelete 未登录或会话已过期";
        SendBadRequestResponse(conn, HttpStatusCode::k401Unauthorized, "未登录或会话已过期");
        return;
    }

    // 2. 从请求中提取要删除的文件名
    std::string fileName = request.GetRequestParamsByKey("filename");
    if (fileName.empty()) 
    {
        SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Missing fileName");
        LOG_WARN << "Missing fileName";
        return;
    }

    // 3. 构造完整文件路径
    std::string filepath = uploadDir_ + "/" + fileName;
    LOG_INFO << "filepath = " << filepath;

    // 4. 查询该用户是否拥有该文件
    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();

    std::string query = "SELECT id FROM files WHERE filename = '" + EscapeString(fileName, mysqlConn->GetRawConnection()) + 
                        "' AND user_id = " + std::to_string(userId);
    MYSQL_RES* result = mysqlConn->Query(query);

    if (!result || mysql_num_rows(result) == 0) 
    {
        if (result) mysql_free_result(result);
        LOG_ERROR << "文件不存在或您没有权限删除此文件";
        SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "文件不存在或您没有权限删除此文件");
        return;
    }

    // 5. 获取文件ID
    MYSQL_ROW row = mysql_fetch_row(result);
    int fileId = std::stoi(row[0]);
    mysql_free_result(result);

    // 6. 删除文件分享记录
    std::string deleteSharesQuery = "DELETE FROM file_shares WHERE file_id = " + std::to_string(fileId);
    if (!mysqlConn->Update(deleteSharesQuery)) 
    {
        LOG_ERROR << "删除文件分享记录失败";
    }

    // 7. 删除文件记录
    std::string deleteFileQuery = "DELETE FROM files WHERE id = " + std::to_string(fileId);
    if (!mysqlConn->Update(deleteFileQuery)) 
    {
        LOG_ERROR << "删除文件记录失败";
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "删除文件记录失败");
        return;
    }

    // 8. 删除文件（如果存在）
    if (access(filepath.c_str(), F_OK) != 0) 
    {
        LOG_WARN << filepath << " not found";
    } 
    else 
    {
        if (unlink(filepath.c_str()) != 0) 
        {
            LOG_WARN << "Failed to delete file: " << filepath << ", error: " << strerror(errno);
        }
        LOG_INFO << "delete file success";
    }

    // 10. 更新文件名映射表（线程安全）
    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        LoadFileNameMapInternal();  // 重新加载映射表
        fileNameMap_.erase(fileName); // 删除映射项
        SaveFileNameMapInternal();   // 保存更新后的映射表
    }

    // 11 构建 JSON 成功响应
    json jsonStr = 
    {
        {"code", 0},
        {"message", "success"}
    };
    response->SetStatusCode(HttpStatusCode::k200OK);
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->AddHeader("Connection", "close");
    response->SetBody(jsonStr.dump());

    // 设置写完成回调，关闭连接
    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());
    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}

// 文件分享处理函数
void HttpServer::HandleShareFile(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    // 1. 验证用户会话是否合法
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");
    int userId;
    std::string usernameFromSession;

    if (!ValidateSession(sessionId, userId, usernameFromSession)) 
    {
        SendBadRequestResponse(conn, HttpStatusCode::k401Unauthorized, "未登录或会话已过期");
        return;
    }

    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    try 
    {
        // 解析请求体中的 JSON 数据
        json requestData = json::parse(request.GetBody());
        int fileId = requestData["fileId"];
        std::string shareType = requestData["shareType"];  // 分享类型: "private", "public", "protected", "user"

        // 验证该文件是否属于当前用户
        std::string fileQuery = "SELECT 1 FROM files WHERE id = " + std::to_string(fileId) +
                                " AND user_id = " + std::to_string(userId);
        MYSQL_RES* fileResult = mysqlConn->Query(fileQuery);
        if (!fileResult || mysql_num_rows(fileResult) == 0) 
        {
            if (fileResult) mysql_free_result(fileResult);
            LOG_WARN << "您没有权限分享此文件";
            SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "您没有权限分享此文件");
            return;
        }
        if (fileResult) mysql_free_result(fileResult);

        // 如果设置为私有，直接删除所有该文件的分享记录并返回
        if (shareType == "private") 
        {
            std::string deleteQuery = "DELETE FROM file_shares WHERE file_id = " + std::to_string(fileId);
            mysqlConn->Query(deleteQuery);

            json jsonStr = {
                {"code", 0},
                {"message", "文件设置为私有成功"}
            };
            response->SetStatusCode(HttpStatusCode::k200OK);
            response->SetStatusMessage("OK");
            response->SetContentType("application/json");
            response->AddHeader("Connection", "close");
            response->SetBody(jsonStr.dump());
            
            const std::string& resp = response->ResponseMessage();
            conn->SendData(resp.data(), resp.size());
            return;
        }

        // 处理分享过期时间（可选字段）
        std::string expireStr = "NULL";
        if (requestData.contains("expireTime") && !requestData["expireTime"].is_null()) 
        {
            int expireHours = requestData["expireTime"];
            if (expireHours > 0) 
            {
                expireStr = "DATE_ADD(NOW(), INTERVAL " + std::to_string(expireHours) + " HOUR)";
            }
        }

        // 生成唯一的分享码（用于 URL 访问）
        std::string shareCode = GenerateShareCode();

        // 初始化共享对象字段
        std::string sharedWithId = "NULL";
        std::string extractCode = "NULL";

        // 处理指定用户分享
        if (shareType == "user" && requestData.contains("sharedWithId")) 
        {
            sharedWithId = std::to_string(requestData["sharedWithId"].get<int>());

            // 检查是否已分享给该用户，避免重复
            std::string checkQuery = "SELECT 1 FROM file_shares WHERE file_id = " + std::to_string(fileId) +
                                     " AND shared_with_id = " + sharedWithId +
                                     " AND share_type = 'user'";
            MYSQL_RES* checkResult = mysqlConn->Query(checkQuery);

            if (checkResult && mysql_num_rows(checkResult) > 0) 
            {
                if (checkResult) mysql_free_result(checkResult);
                LOG_WARN << "已经分享给该用户";
                SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "已经分享给该用户");
                return;
            }
            if (checkResult) mysql_free_result(checkResult);
        }
        // 处理受保护分享（生成提取码）
        else if (shareType == "protected") 
        {
            extractCode = "'" + GenerateExtractCode() + "'";
        }

        // 插入分享记录
        std::string insertQuery = "INSERT INTO file_shares (file_id, owner_id, shared_with_id, share_type, share_code, extract_code, expire_time) VALUES (" +
                                  std::to_string(fileId) + ", " +
                                  std::to_string(userId) + ", " +
                                  sharedWithId + ", '" +
                                  EscapeString(shareType, mysqlConn->GetRawConnection()) + "', '" +
                                  EscapeString(shareCode, mysqlConn->GetRawConnection()) + "', " +
                                  extractCode + ", " +
                                  expireStr + ")";

        int shareID = mysqlConn->Update(insertQuery);
        if (!shareID) 
        {
            LOG_ERROR << "创建分享失败";
            SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "创建分享失败");
            return;
        }

        // 构建返回的 JSON 响应体
        json jsonStr = {
            {"code", 0},
            {"message", "分享成功"},
            {"shareId", shareID},
            {"shareType", shareType},
            {"shareCode", shareCode},
            {"shareLink", "/share/" + shareCode}
        };

        if (shareType == "user") 
        {
            jsonStr["sharedWithId"] = std::stoi(sharedWithId);
        } 
        else if (shareType == "protected") 
        {
            // 去除引号包裹的提取码
            jsonStr["extractCode"] = extractCode.substr(1, extractCode.length() - 2);
        }

        // 设置 HTTP 响应内容
        response->SetStatusCode(HttpStatusCode::k200OK);
        response->SetStatusMessage("OK");
        response->SetContentType("application/json");
        response->AddHeader("Connection", "close");
        response->SetBody(jsonStr.dump());

        const std::string& resp = response->ResponseMessage();
        conn->SendData(resp.data(), resp.size());
        conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR << "分享文件错误: " << e.what();
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "分享失败: " + std::string(e.what()));
        return;
    }
}

// 通过分享码访问文件
void HttpServer::HandleShareAccess(const spConnection &conn, HttpRequest &request, HttpResponse *response) 
{
    // 获取请求路径
    std::string path = request.GetUrl();
    std::smatch matches;
    std::regex codeRegex("/share/([^/]+)");  // 匹配/share/后面的分享码
    LOG_INFO << "path = " << path;

    // 正则提取分享码
    if (!std::regex_search(path, matches, codeRegex) || matches.size() < 2) 
    {
        SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "无效的分享链接");
        LOG_WARN << "invalid share link";
        return;
    }

    std::string shareCode = matches[1];
    std::string acceptHeader = request.GetHeader("Accept");

    // 会话验证（可选）
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");
    int userId = 0;
    std::string username;
    bool isAuthenticated = ValidateSession(sessionId, userId, username);

    // 判断是否为 AJAX 请求
    if (request.GetHeader("X-Requested-With") == "XMLHttpRequest" || 
        acceptHeader.find("application/json") != std::string::npos) 
    {
        LOG_INFO << "AJAX请求，返回文件信息, shareCode = " << shareCode;

        // 检查分享码格式
        if (shareCode.empty() || shareCode.length() != 32) 
        {
            LOG_WARN << "无效的分享码格式";
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "无效的分享码格式");
            return;
        }

        // 检查是否包含非法字符
        if (!std::all_of(shareCode.begin(), shareCode.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        })) 
        {
            LOG_WARN << "分享码包含非法字符";
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "分享码包含非法字符");
            return;
        }

        // 获取提取码（可选）
        std::string extractCode = request.GetRequestParamsByKey("code");

        // 查询分享信息（文件 + 用户）
        std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
        std::string query = "SELECT fs.*, f.filename, f.original_filename, f.file_size, "
                            "f.file_type, u.username as owner_username, f.user_id "
                            "FROM file_shares fs "
                            "JOIN files f ON fs.file_id = f.id "
                            "JOIN users u ON f.user_id = u.id "
                            "WHERE fs.share_code = '" + EscapeString(shareCode, mysqlConn->GetRawConnection()) + "' "
                            "AND (fs.expire_time IS NULL OR fs.expire_time > NOW()) "
                            "AND (fs.share_type != 'protected' OR (fs.share_type = 'protected' AND fs.extract_code = '" + EscapeString(extractCode, mysqlConn->GetRawConnection()) + "'))";

        LOG_INFO << "query = " << query;
        MYSQL_RES* result = mysqlConn->Query(query);

        if (!result || mysql_num_rows(result) == 0) 
        {
            if (result) mysql_free_result(result);
            LOG_ERROR << "分享链接已失效或不存在";
            SendBadRequestResponse(conn, HttpStatusCode::k404NotFound, "分享链接已失效或不存在");
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(result);
        std::string shareType = row[4];  // fs.share_type
        bool isOwner = (row[13] && std::stoi(row[13]) == userId);  // 当前用户是否为文件所有者
        int sharedWithId = row[3] ? std::stoi(row[3]) : 0;         // fs.shared_with_id
        std::string dbExtractCode = row[5] ? row[5] : "";          // fs.extract_code

        // 权限判断逻辑
        bool hasPermission = false;
        if (isOwner) 
        {
            hasPermission = true;
        } 
        else if (shareType == "public") 
        {
            hasPermission = true;
        } 
        else if (shareType == "protected") 
        {
            if (!extractCode.empty() && extractCode == dbExtractCode) 
            {
                hasPermission = true;
            }
        } 
        else if (shareType == "user") 
        {
            if (isAuthenticated && userId == sharedWithId) 
            {
                hasPermission = true;
            }
        }

        if (!hasPermission) 
        {
            mysql_free_result(result);
            if (shareType == "protected" && (extractCode.empty() || extractCode != dbExtractCode)) 
            {
                LOG_ERROR << "需要正确的提取码";
                SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "需要正确的提取码");
            } 
            else 
            {
                LOG_ERROR << "您没有权限访问此文件";
                SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "您没有权限访问此文件");
            }
            return;
        }

        // 构造 JSON 响应
        json jsonStr = {
            {"code", 0},
            {"message", "success"},
            {"file", {
                {"id", std::stoi(row[0])},
                {"fileId", std::stoi(row[1])},
                {"ownerId", std::stoi(row[2])},
                {"sharedWithId", row[3] ? std::stoi(row[3]) : 0},
                {"shareType", shareType},
                {"shareCode", shareCode},
                {"createdAt", row[6] ? row[6] : ""},
                {"expireTime", row[7] ? row[7] : ""},
                {"filename", row[8] ? row[8] : ""},
                {"originalName", row[9] ? row[9] : ""},
                {"size", row[10] ? std::stoull(row[10]) : 0},
                {"type", row[11] ? row[11] : "unknown"},
                {"ownerUsername", row[12] ? row[12] : ""},
                {"isOwner", isOwner}
            }},
            {"downloadUrl", "/share/download/" + std::string(row[8] ? row[8] : "") + "?code=" + shareCode}
        };

        mysql_free_result(result);

        response->SetStatusCode(HttpStatusCode::k200OK);
        response->SetStatusMessage("OK");
        response->SetContentType("application/json");
        response->SetBody(jsonStr.dump());
    } 
    else 
    {
        // 非AJAX请求，返回分享页面
        LOG_INFO << "返回分享页面 share.html";
        HandleIndex(conn, request, response);  // 显示 share.html 页面
        response->AddHeader("X-Share-Code", shareCode);
        return;
    }

    response->AddHeader("Connection", "close");

    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());
    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}


// 处理通过分享链接下载文件（支持权限判断与断点续传）
void HttpServer::HandleShareDownload(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    response->SetAsync(true);
    // 获取路径参数中的文件名
    std::string filename = request.GetRequestParamsByKey("filename");
    if (filename.empty()) 
    {
        SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Missing filename");
        return;
    }

    // 获取 URL 中的分享码和提取码
    std::string shareCode = request.GetRequestParamsByKey("code");
    std::string extractCode = request.GetRequestParamsByKey("extract_code");
    if (shareCode.empty()) 
    {
        LOG_ERROR << "Missing share code";
        SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Missing share code");
        return;
    }

    // 尝试从请求头获取 sessionId
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");
    int userId = 0;
    std::string usernameFromSession;
    bool isAuthenticated = ValidateSession(sessionId, userId, usernameFromSession);

    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    // 构建查询语句以查找对应的分享信息
    std::string query = "SELECT f.id, f.filename, f.original_filename, f.user_id, "
                        "fs.share_type, fs.shared_with_id, fs.extract_code "
                        "FROM files f "
                        "JOIN file_shares fs ON f.id = fs.file_id "
                        "WHERE f.filename = '" + EscapeString(filename, mysqlConn->GetRawConnection()) + "' "
                        "AND fs.share_code = '" + EscapeString(shareCode, mysqlConn->GetRawConnection()) + "' "
                        "AND (fs.expire_time IS NULL OR fs.expire_time > NOW())";

    MYSQL_RES* result = mysqlConn->Query(query);
    if (!result || mysql_num_rows(result) == 0) 
    {
        if (result) mysql_free_result(result);
        LOG_ERROR << "Share not found or expired";
        SendBadRequestResponse(conn, HttpStatusCode::k404NotFound, "Share not found or expired");
        return;
    }

    // 解析分享记录字段
    MYSQL_ROW row = mysql_fetch_row(result);
    std::string serverFilename = row[1];
    std::string originalFilename = row[2];
    int fileOwnerId = std::stoi(row[3]);
    std::string shareType = row[4];
    int sharedWithId = row[5] ? std::stoi(row[5]) : 0;
    std::string dbExtractCode = row[6] ? row[6] : "";
    mysql_free_result(result);

    // 权限判断
    bool hasPermission = false;
    if (isAuthenticated && userId == fileOwnerId) 
    {
        hasPermission = true; // 所有者可访问
    } 
    else if (shareType == "public") 
    {
        hasPermission = true; // 公开分享
    } 
    else if (shareType == "protected" && extractCode == dbExtractCode) 
    {
        hasPermission = true; // 需要正确的提取码
    } 
    else if (shareType == "user" && isAuthenticated && userId == sharedWithId) 
    {
        hasPermission = true; // 指定用户
    }

    if (!hasPermission) 
    {
        LOG_ERROR << "无权限访问此文件";
        SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "无权限访问此文件");
        return;
    }

    // 构造文件路径
    std::string filepath = uploadDir_ + "/" + serverFilename;
    if (!fs::exists(filepath) || !fs::is_regular_file(filepath)) 
    {
        LOG_ERROR << "File not found file";
        SendBadRequestResponse(conn, HttpStatusCode::k404NotFound, "File not found");
        return;
    }

    uintmax_t fileSize = fs::file_size(filepath); // 获取文件大小
    uintmax_t startPos = 0, endPos = fileSize - 1;
    bool isRangeRequest = false;

    // 解析 Range 请求头（支持断点续传）
    std::string rangeHeader = request.GetHeader("Range");
    if (!rangeHeader.empty()) 
    {
        std::regex rangeRegex("bytes=(\\d+)-(\\d*)");
        std::smatch matches;
        if (std::regex_search(rangeHeader, matches, rangeRegex)) 
        {
            startPos = std::stoull(matches[1]);
            if (matches[2].matched && !matches[2].str().empty()) 
            {
                endPos = std::stoull(matches[2]);
            }
            if (startPos >= fileSize) 
            {
                LOG_ERROR << "Range Not Satisfiable";
                SendBadRequestResponse(conn, HttpStatusCode::k416RangeNotSatisfiable, "Range Not Satisfiable");
                return;
            }
            if (endPos >= fileSize) 
            {
                endPos = fileSize - 1;
            }
            isRangeRequest = true;
        }
    }

    // 获取连接上下文（HttpContext）
    auto httpContext = std::static_pointer_cast<HttpContext>(conn->GetContext());
    if (!httpContext) 
    {
        LOG_ERROR << "Internal Server Error";
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "Internal Server Error");
        return;
    }

    // 尝试获取已存在的下载上下文
    std::shared_ptr<FileDownContext> downContext = httpContext->GetContext<FileDownContext>();
    if (!downContext) 
    {
        // 创建新下载上下文并绑定到 HttpContext
        downContext = std::make_shared<FileDownContext>(filepath, originalFilename);
        httpContext->SetContext(downContext);

        // 构造响应头
        if (isRangeRequest) 
        {
            response->SetStatusCode(HttpStatusCode::k206PartialContent);
            response->SetStatusMessage("Partial Content");
            response->AddHeader("Content-Range", "bytes " + std::to_string(startPos) + "-" +
                                               std::to_string(endPos) + "/" +
                                               std::to_string(fileSize));
        } 
        else 
        {
            response->SetStatusCode(HttpStatusCode::k200OK);
            response->SetStatusMessage("OK");
        }

        response->SetContentType("application/octet-stream");
        response->AddHeader("Content-Disposition", "attachment; filename=\"" + originalFilename + "\"");
        response->AddHeader("Transfer-Encoding", "chunked");  // 关键设置
        response->AddHeader("Accept-Ranges", "bytes");
        response->AddHeader("Connection", "keep-alive");

        // 发送消息头部
        const std::string& resp = response->ResponseMessage();
        conn->SendData(resp.data(), resp.size());

        downContext->SeekTo(startPos); // 定位文件指针
    }

    // 继续读取数据并发送给客户端
    // 设置发送完成回调
    conn->SetSendCompleteCallback([this, downContext](spConnection c) 
    {
        std::string chunk;
        if (downContext->ReadNextChunk(chunk)) {
            // 格式：<chunk-size>\r\n<chunk-data>\r\n
            std::ostringstream oss;
            oss << std::hex << chunk.size() << "\r\n" << chunk << "\r\n";
            std::string chunkedData = oss.str();
            c->SendData(chunkedData.data(), chunkedData.size());
        } 
        else 
        {
            // 结尾块：0\r\n\r\n，表示发送完成
            c->SendData("0\r\n\r\n", 5);
            c->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
        }
    });
}

// 处理获取分享信息请求（校验分享码、提取码，并返回文件元信息）
void HttpServer::HandleShareInfo(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    // 从路径参数中获取分享码
    std::string shareCode = request.GetRequestParamsByKey("code");
    if (shareCode.empty()) 
    {
        SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "Missing share code");
        return;
    }

    // 可选：提取码，仅用于 "protected" 类型分享
    std::string extractCode = request.GetRequestParamsByKey("extract_code");
    LOG_INFO << "shareCode = " << shareCode << ", extractCode = " << extractCode;

    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    // 构造 SQL 查询，获取分享信息 + 文件信息 + 拥有者用户名
    std::string query = "SELECT fs.*, f.filename, f.original_filename, f.file_size, "
                        "f.file_type, u.username AS owner_username, f.user_id "
                        "FROM file_shares fs "
                        "JOIN files f ON fs.file_id = f.id "
                        "JOIN users u ON f.user_id = u.id "
                        "WHERE fs.share_code = '" + EscapeString(shareCode, mysqlConn->GetRawConnection()) + "' "
                        "AND (fs.expire_time IS NULL OR fs.expire_time > NOW())";

    LOG_INFO << "查询分享信息: " << query;
    MYSQL_RES* result = mysqlConn->Query(query);
    if (!result || mysql_num_rows(result) == 0) 
    {
        if (result) mysql_free_result(result);
        LOG_ERROR << "分享链接已失效或不存在, shareCode = " << shareCode;
        SendBadRequestResponse(conn, HttpStatusCode::k404NotFound, "分享链接已失效或不存在");
        return;
    }

    // 提取查询结果字段（按顺序索引）
    MYSQL_ROW row = mysql_fetch_row(result);
    std::string shareType = row[4]; // fs.share_type
    std::string dbExtractCode = row[8] ? row[8] : ""; // fs.extract_code
    std::string serverFilename = row[9] ? row[9] : ""; // f.filename
    std::string originalFilename = row[10] ? row[10] : ""; // f.original_filename
    uintmax_t fileSize = row[11] ? std::stoull(row[11]) : 0; // f.file_size
    std::string fileType = row[12] ? row[12] : "unknown"; // f.file_type
    std::string createdAt = row[7] ? row[7] : ""; // fs.created_at
    std::string expireTime = row[6] ? row[6] : ""; // fs.expire_time

    // 若分享类型为 protected，需校验提取码
    if (shareType == "protected") 
    {
        if (extractCode.empty() || extractCode != dbExtractCode) 
        {
            mysql_free_result(result);
            LOG_ERROR << "提取码错误或未提供, shareCode = " << shareCode;
            SendBadRequestResponse(conn, HttpStatusCode::k403Forbidden, "需要正确的提取码");
            return;
        }
    }

    mysql_free_result(result);

    // 构建响应 JSON
    json jsonStr = {
        {"code", 0},
        {"message", "success"},
        {"shareType", shareType},
        {"file", {
            {"id", std::stoi(row[1])}, // fs.file_id
            {"name", serverFilename},
            {"originalName", originalFilename},
            {"size", fileSize},
            {"type", fileType},
            {"shareTime", createdAt},
            {"expireTime", expireTime}
        }}
    };

    // 返回 JSON 响应
    response->SetStatusCode(HttpStatusCode::k200OK);
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->AddHeader("Connection", "close");
    response->SetBody(jsonStr.dump());

    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());
    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}

// 处理 favicon.ico 请求（返回浏览器标签页图标）
void HttpServer::HandleFavicon(const spConnection &conn, HttpRequest &request, HttpResponse *response) 
{
    // 获取当前源文件的路径（__FILE__ 是预定义宏，表示当前源文件绝对路径）
    std::string currentDir = __FILE__;

    // 提取当前目录（从文件路径中去掉文件名部分）
    std::string::size_type pos = currentDir.find_last_of('/');
    std::string projectRoot = (pos != std::string::npos) ? currentDir.substr(0, pos) : ".";

    // 构造 favicon.ico 的完整路径（假设位于项目根目录）
    std::string faviconPath = projectRoot + "/favicon.ico";

    // 打开 favicon 文件（二进制方式读取）
    std::ifstream file(faviconPath, std::ios::binary);
    if (!file.is_open()) 
    {
        // 打开失败，返回 404 响应
        LOG_ERROR << "Failed to open favicon.ico";
        response->SetStatusCode(HttpStatusCode::k404NotFound);
        response->SetStatusMessage("Not Found");
        response->SetContentType("image/x-icon");
        response->AddHeader("Connection", "close");
        response->SetBody("");
    } 
    else 
    {
        // 读取文件全部内容到字符串
        std::string iconData((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        // 构造成功响应，设置图标数据
        response->SetStatusCode(HttpStatusCode::k200OK);
        response->SetStatusMessage("OK");
        response->SetContentType("image/x-icon");  // 设置 MIME 类型
        response->AddHeader("Connection", "close");
        response->SetBody(iconData);
    }

    // 设置写完成回调，关闭连接
    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());
    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}

// 用户注册
void HttpServer::HandleRegister(const spConnection &conn, HttpRequest &request, HttpResponse *response) 
{
    LOG_INFO << "Handling register request";
    LOG_INFO << "Request body: " << request.GetBody();

    try 
    {
        // 1. 解析 JSON 请求体
        json requestData = json::parse(request.GetBody());
        std::string username = requestData["username"];
        std::string password = requestData["password"];
        std::string email = requestData.value("email", ""); // 可选字段

        LOG_INFO << "Register attempt for username: " << username;

        // 2. 验证用户名和密码不为空
        if (username.empty() || password.empty()) 
        {
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "用户名和密码不能为空");
            return;
        }

        // 3. 对密码进行哈希处理（真实项目建议用 bcrypt/argon2 等）
        std::string hashedPassword = sha256(password);

        // 4. 查询数据库是否存在相同用户名
        std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
        MYSQL* mysql = mysqlConn->GetRawConnection();
        
        std::string checkQuery = "SELECT id FROM users WHERE username = '" + 
                                 EscapeString(username, mysql) + "'";
        MYSQL_RES* result = mysqlConn->Query(checkQuery);

        if (result && mysql_num_rows(result) > 0) 
        {
            mysql_free_result(result);
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "用户名已存在");
            return;
        }
        if (result) 
        {
            mysql_free_result(result);
        }

        
        // 5. 插入新用户记录（email 允许为空）
        std::string insertQuery = "INSERT INTO users (username, password, email) VALUES ('" +
                                  EscapeString(username, mysql) + "', '" +
                                  EscapeString(hashedPassword, mysql) + "', " +
                                  (email.empty() ? "NULL" : ("'" + EscapeString(email, mysql) + "'")) + ")";

        int userID = mysqlConn->Update(insertQuery);
        if (!userID) 
        {
            SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "注册失败，请稍后重试");
            return;
        }

        // 6. 构造响应 JSON
        json jsonStr = 
        {
            {"code", 0},
            {"message", "注册成功"},
            {"userId", userID}
        };

        // 7. 设置 HTTP 响应信息
        response->SetStatusCode(HttpStatusCode::k200OK);
        response->SetStatusMessage("OK");
        response->SetContentType("application/json");
        response->AddHeader("Connection", "close");
        response->SetBody(jsonStr.dump());

        // 8. 设置连接写完成回调，关闭连接
        const std::string& resp = response->ResponseMessage();
        conn->SendData(resp.data(), resp.size());
        conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
    } 
    catch (const std::exception& e) 
    {
        LOG_ERROR << "用户注册错误: " << e.what();
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "注册失败: " + std::string(e.what()));
        return;
    }
}

// 登录
void HttpServer::HandleLogin(const spConnection &conn, HttpRequest &request, HttpResponse *response) 
{
    try 
    {
        // 解析请求体为 JSON 格式，提取用户名和密码
        json requestData = json::parse(request.GetBody());
        std::string username = requestData["username"];
        std::string password = requestData["password"];

        // 校验输入参数
        if (username.empty() || password.empty()) 
        {
            SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "用户名和密码不能为空");
            return;
        }

        // 对密码进行哈希（注意：此处为演示，实际应使用真正的密码学算法）
        std::string hashedPassword = sha256(password);

        // 查询数据库中是否存在该用户，用户名+密码匹配
        std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
        std::string query = "SELECT id, username FROM users WHERE username = '" +
                            EscapeString(username, mysqlConn->GetRawConnection()) + "' AND password = '" +
                            EscapeString(hashedPassword, mysqlConn->GetRawConnection()) + "'";

        MYSQL_RES* result = mysqlConn->Query(query);
        // 没查到或出错则返回错误
        if (!result || mysql_num_rows(result) == 0) 
        {
            if (result) mysql_free_result(result);
            LOG_ERROR << "用户名或密码错误";
            SendBadRequestResponse(conn, HttpStatusCode::k401Unauthorized, "用户名或密码错误");
            return;
        }

        // 获取查询结果，用户 ID 和用户名
        MYSQL_ROW row = mysql_fetch_row(result);
        int userId = std::stoi(row[0]);
        std::string usernameFromDb = row[1];
        mysql_free_result(result);

        // 生成随机 sessionId 并保存会话
        std::string sessionId = GenerateSessionId();       // 应使用随机值生成函数
        SaveSession(sessionId, userId, usernameFromDb);    // 保存到数据库

        // 构造 JSON 成功响应
        json jsonStr = {
            {"code", 0},
            {"message", "登录成功"},
            {"sessionId", sessionId},
            {"userId", userId},
            {"username", usernameFromDb}
        };

        // 设置响应头和响应体
        response->SetStatusCode(HttpStatusCode::k200OK);
        response->SetStatusMessage("OK");
        response->SetContentType("application/json");
        response->AddHeader("Connection", "close");
        // 设置 cookie 让浏览器自动携带 sessionId
        response->AddHeader("Set-Cookie", "session_id=" + sessionId + "; Path=/; HttpOnly");
        response->SetBody(jsonStr.dump());

        // 设置写回调关闭连接
        const std::string& resp = response->ResponseMessage();
        conn->SendData(resp.data(), resp.size());
        return;
    } 
    catch (const std::exception& e) 
    {
        // 捕获 JSON 解析或 DB 操作异常
        LOG_ERROR << "用户登录错误: " << e.what();
        SendBadRequestResponse(conn, HttpStatusCode::k500InternalServerError, "登录失败: " + std::string(e.what()));
        return;
    }
}

// 退出请求处理函数
void HttpServer::HandleLogout(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    // 从请求头获取会话 ID
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");

    // 如果存在 sessionId，则清除对应的会话记录
    if (!sessionId.empty()) 
    {
        DeleteSession(sessionId);  // 从 sessions 表中删除此 session 记录
    }

    // 构造 JSON 格式的登出成功响应
    json jsonStr = {
        {"code", 0},
        {"message", "Logout successful"}
    };

    // 设置 HTTP 响应头和内容
    response->SetStatusCode(HttpStatusCode::k200OK);
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->AddHeader("Connection", "close");
    response->SetBody(jsonStr.dump());

    // 设置连接写完成后自动关闭连接
    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());
    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}

// 处理搜索用户请求
void HttpServer::HandleSearchUsers(const spConnection &conn, HttpRequest &request, HttpResponse *response)
{
    // 从请求头中提取 Session ID 并验证用户身份
    std::string cookie = request.GetHeader("Cookie");
    std::string sessionId = ParseCookie(cookie, "session_id");
    int userId;
    std::string username;
    
    if (!ValidateSession(sessionId, userId, username)) 
    {
        SendBadRequestResponse(conn, HttpStatusCode::k401Unauthorized, "未登录或会话已过期");
        LOG_WARN << "validateSession failed";
        return;
    }

    // 获取原始查询字符串值，示例格式：?keyword=alice
    std::string keyword = request.GetRequestParamsByKey("keyword");

    // 检查 keyword 是否为空
    if (keyword.empty()) 
    {
        SendBadRequestResponse(conn, HttpStatusCode::k400BadRequest, "搜索关键词不能为空");
        LOG_WARN << "keyword is empty";
        return;
    }

    // 构造模糊搜索 SQL 查询（排除当前用户）
    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    std::string sqlQuery = "SELECT id, username, email FROM users WHERE username LIKE '%" +
                           EscapeString(keyword, mysqlConn->GetRawConnection()) + "%' AND id != " + std::to_string(userId) +
                           " LIMIT 10";
    LOG_INFO << "sqlQuery = " << sqlQuery;

    // 执行 SQL 查询
    MYSQL_RES* result = mysqlConn->Query(sqlQuery);

    // 构造响应 JSON
    json jsonStr;
    jsonStr["code"] = 0;
    jsonStr["message"] = "Success";
    json users = json::array();

    // 遍历查询结果，添加用户信息到数组
    if (result) 
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) 
        {
            int id = std::stoi(row[0]);
            std::string usernameFromDb = row[1] ? row[1] : "";
            std::string email = row[2] ? row[2] : "";

            users.push_back({
                {"id", id},
                {"username", usernameFromDb},
                {"email", email}
            });
        }
        mysql_free_result(result);
    }

    // 设置最终响应内容
    jsonStr["users"] = users;
    response->SetStatusCode(HttpStatusCode::k200OK);
    response->SetStatusMessage("OK");
    response->SetContentType("application/json");
    response->AddHeader("Connection", "close");
    response->SetBody(jsonStr.dump());

    LOG_INFO << "response = " << jsonStr.dump();

    // 写完成后关闭连接
    const std::string& resp = response->ResponseMessage();
    conn->SendData(resp.data(), resp.size());

    conn->SetSendCompleteCallback(std::bind(&HttpServer::HandleSendComplete, this, std::placeholders::_1));
}

// 加载文件映射
void HttpServer::LoadFileNameMap() 
{
    std::lock_guard<std::mutex> lock(mapMutex_);
    LoadFileNameMapInternal();
}

// 内部函数，不加锁，加载文件名映射
void HttpServer::LoadFileNameMapInternal() 
{
     try 
    {
        if (fs::exists(mapFile_)) 
        {
            std::ifstream file(mapFile_);
            fileNameMap_ = json::parse(file).get<std::map<std::string, std::string>>();
        }
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Failed to load filename mapping: " << e.what();
    }
}

// 保存文件名映射
void HttpServer::SaveFileNameMap() 
{
    // 使用锁保护，防止多线程访问时发生数据竞争
    std::lock_guard<std::mutex> lock(mapMutex_);
    SaveFileNameMapInternal();
}

void HttpServer::SaveFileNameMapInternal() 
{
    try 
    {
        // 打开文件进行写入，若文件不存在会创建文件
        std::ofstream file(mapFile_);
        
        // 将 filenameMap_ 序列化为 JSON 格式，并格式化为 2 个空格的缩进
        file << json(fileNameMap_).dump(2);
    } 
    catch (const std::exception& e) 
    {
        // 捕获异常并打印错误日志
        LOG_ERROR << "Failed to save filename map: " << e.what();
    }
}

// 初始化路由表
void HttpServer::InitRoutes() 
{
    // 不需要会话验证的路由
    AddRoute("/favicon.ico", Method::kGet, &HttpServer::HandleFavicon);
    AddRoute("/register", Method::kPost, &HttpServer::HandleRegister);
    AddRoute("/login", Method::kPost, &HttpServer::HandleLogin);
    AddRoute("/", Method::kGet, &HttpServer::HandleIndex);
    AddRoute("/index.html", Method::kGet, &HttpServer::HandleIndex);
    AddRoute("/register.html", Method::kGet, &HttpServer::HandleIndex);
    AddRoute("/share/([^/]+)", Method::kGet, &HttpServer::HandleShareAccess, {"code"});
    AddRoute("/share/download/([^/]+)", Method::kGet, &HttpServer::HandleShareDownload, {"filename"});
    AddRoute("/share/info/([^/]+)", Method::kGet, &HttpServer::HandleShareInfo, {"code"});
    
    // 需要会话验证的路由
    AddRoute("/upload", Method::kPost, &HttpServer::HandleFileUpload);
    AddRoute("/files", Method::kGet, &HttpServer::HandleListFiles);
    AddRoute("/download/([^/]+)", Method::kHead, &HttpServer::HandleDownload, {"filename"});
    AddRoute("/download/([^/]+)", Method::kGet, &HttpServer::HandleDownload, {"filename"});
    AddRoute("/delete/([^/]+)", Method::kDelete, &HttpServer::HandleDelete, {"filename"});
    AddRoute("/share", Method::kPost, &HttpServer::HandleShareFile);
    AddRoute("/users/search", Method::kGet, &HttpServer::HandleSearchUsers);
    AddRoute("/logout", Method::kPost, &HttpServer::HandleLogout);
}

// 添加精确匹配的路由
void HttpServer::AddRoute(const std::string& path, Method method, RequestHandler handler)
{
    // 将路径转换为正则表达式，确保路径完全匹配
    std::string pattern = "^" + EscapeRegex(path) + "$";  // escapeRegex 用于转义路径中的特殊字符
    routes_.emplace_back(pattern, std::vector<std::string>(), handler, method);  // 将路由添加到路由表
}

// 添加带参数的路由
void HttpServer::AddRoute(const std::string& pattern, Method method, 
             RequestHandler handler, const std::vector<std::string>& paramNames) 
{
    // 将带有参数的路由添加到路由表
    routes_.emplace_back(pattern, paramNames, handler, method);
}

// 保存用户会话信息到数据库
void HttpServer::SaveSession(const std::string& sessionId, int userId, const std::string& username) 
{
    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    // 构造 SQL 插入语句，将会话 ID、用户 ID、用户名和过期时间插入 sessions 表
    std::string query = "INSERT INTO sessions (session_id, user_id, username, expire_time) VALUES ('" +
                        EscapeString(sessionId, mysqlConn->GetRawConnection()) + "', " +                  // 防止 SQL 注入
                        std::to_string(userId) + ", '" +
                        EscapeString(username, mysqlConn->GetRawConnection()) + "', " +
                        "DATE_ADD(NOW(), INTERVAL 30 MINUTE))";           // 设置过期时间为当前时间 + 30分钟

    // 执行 SQL 插入语句
    mysqlConn->Query(query);
}

// 验证会话，检查 sessionId 是否有效
bool HttpServer::ValidateSession(const std::string& sessionId, int& userId, std::string& username) 
{
    // 1. 如果 sessionId 为空，直接返回 false
    if (sessionId.empty()) 
    {
        LOG_WARN << "sessionId is empty";  // 记录警告日志
        return false;
    }

    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    // 2. 查询数据库，验证会话是否存在且未过期
    std::string query = "SELECT user_id, username FROM sessions WHERE session_id = '" +
                        EscapeString(sessionId, mysqlConn->GetRawConnection()) + "' AND expire_time > NOW()";
    
    // 执行查询并获取结果
    MYSQL_RES* result = mysqlConn->Query(query);
    
    // 3. 如果查询失败或没有找到结果，返回 false
    if (!result || mysql_num_rows(result) == 0) 
    {
        if (result) 
        {
            mysql_free_result(result);  // 释放查询结果
        }
        LOG_WARN << "mysql_num_rows is invalid";  // 记录警告日志
        return false;
    }

    // 4. 获取查询结果，填充 userId 和 username
    MYSQL_ROW row = mysql_fetch_row(result);
    userId = std::stoi(row[0]);  // 将查询结果中的 user_id 转换为 int
    username = row[1];           // 获取用户名

    // 5. 释放查询结果
    mysql_free_result(result);

    // 6. 更新会话过期时间，延长 30 分钟
    std::string updateQuery = "UPDATE sessions SET expire_time = DATE_ADD(NOW(), INTERVAL 30 MINUTE) WHERE session_id = '" +
                              EscapeString(sessionId, mysqlConn->GetRawConnection()) + "'";
    mysqlConn->Update(updateQuery);  // 执行更新会话过期时间的查询

    LOG_INFO << "validateSession success";  // 记录日志
    return true;  // 返回会话有效
}

// 结束会话，删除 session 数据
void HttpServer::DeleteSession(const std::string& sessionId) 
{
    // 1. 如果 sessionId 为空，直接返回
    if (sessionId.empty()) 
    {
        return;
    }

    // 2. 删除会话
    std::shared_ptr<MySqlConnection> mysqlConn = mysqlPool_->GetConnection();
    std::string query = "DELETE FROM sessions WHERE session_id = '" + EscapeString(sessionId, mysqlConn->GetRawConnection()) + "'";
    mysqlConn->Update(query);  // 执行删除会话的查询
}





