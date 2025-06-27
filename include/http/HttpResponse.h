#ifndef HTTPRESPONSE_H
#define HTTPRESPONSE_H

#include <string>
#include <map>

#include <string>
#include <unordered_map>
#include <iostream>

// HTTP 状态码枚举
enum class HttpStatusCode 
{
    kUnknown = 0,                   // 未知状态，默认初始化值
    k100Continue = 100,             // 继续：客户端应继续其请求
    k200OK = 200,                   // 成功：请求已成功处理
    k201Created = 201,              // 已创建：请求成功，资源已被创建
    k204NoContent = 204,            // 无内容：请求成功，但响应中无正文内容
    k206PartialContent = 206,       // 部分内容：响应中包含部分资源（常用于断点续传）
    k302Found = 302,                // 临时重定向：请求的资源临时被移动到另一个 UR
    k400BadRequest = 400,           // 错误请求：服务器无法理解请求（如语法错误）
    k401Unauthorized = 401,         // 未授权：请求需要身份验证
    k403Forbidden = 403,            // 禁止访问：服务器理解请求但拒绝执行
    k404NotFound = 404,             // 未找到：请求的资源不存在
    k405MethodNotAllowed = 405,     // 方法不被允许：请求方法对资源无效
    k416RangeNotSatisfiable = 416,  // 范围无效：客户端请求的资源范围无效或超出范围（常用于下载）
    k500InternalServerError = 500   // 服务器内部错误：服务器遇到意外情况，无法完成请求

};

class HttpResponse 
{
private:
    HttpStatusCode statusCode_;
    std::string statusMessage_;
    std::string version_;
    bool closeConnection_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    bool isAsync_;
    
public:
    explicit HttpResponse(bool close_connection);
    ~HttpResponse();

    void SetStatusCode(HttpStatusCode statusCode);
    void SetStatusMessage(const std::string& statusMessage);
    void SetVersion(const std::string& version);
    void SetCloseConnection(bool closeConnection);
    void SetContentType(const std::string& contentType);
    void SetBody(const std::string& body);
    void AddHeader(const std::string& key, const std::string& value);
    void AddSetCookie(const std::string& cookie);
    void AddDateHeader();
    bool IsCloseConnection() const;
    std::string ResponseMessage();         // 构造完整 HTTP 响应字符串
    void DebugPrint() const;               // 打印响应内容（调试用）
    void SetAsync(bool async) { isAsync_ = async; }
    bool IsAsync() const { return isAsync_; }

private:
    std::string GetStatusLine() const;     // 响应行
    static std::string GetDefaultStatusMessage(HttpStatusCode code);
};

#endif