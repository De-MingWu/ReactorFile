#ifndef PUBLIC_H
#define PUBLIC_H

#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>
#include <random>
#include <iomanip>
#include <string>
#include <mysql.h>

static std::string ParseCookie(const std::string& cookieHeader, const std::string& key) 
{
    std::regex re(key + "=([^;]+)");
    std::smatch match;
    if (std::regex_search(cookieHeader, match, re) && match.size() > 1) 
    {
        return match[1];
    }
    return "";
}

// 转义正则表达式特殊字符
std::string EscapeRegex(const std::string& str) 
{
    std::string result;
    // 遍历路径中的每个字符，将正则表达式特殊字符进行转义
    for (char c : str) 
    {
        if (c == '.' || c == '+' || c == '*' || c == '?' || c == '^' || 
            c == '$' || c == '(' || c == ')' || c == '[' || c == ']' || 
            c == '{' || c == '}' || c == '|' || c == '\\') 
        {
            result += '\\';  // 添加转义符
        }
        result += c;  // 将字符加入结果字符串
    }
    return result;
}

// 将 URL 编码的字符串转换为原始字符
static std::string URLDecode(const std::string& encoded) 
{
    std::string result;
    size_t len = encoded.length();

    for (size_t i = 0; i < len; ++i) 
    {
        if (encoded[i] != '%') 
        {
            // 将 '+' 替换为空格
            if (encoded[i] == '+') 
            {
                result += ' ';
            } 
            else 
            {
                result += encoded[i];
            }
        } 
        else 
        {
            // 处理 '%xx' 形式的编码
            if (i + 2 < len) 
            {
                // 提取 '%' 后的两位十六进制数
                std::string hexStr = encoded.substr(i + 1, 2);
                int value;
                std::istringstream(hexStr) >> std::hex >> value;
                result += static_cast<char>(value);  // 转换为字符并添加到结果中
                i += 2;  // 跳过处理过的两个字符
            }
        }
    }
    return result;
}

// 生成一个 唯一的文件名
static std::string GenerateUniqueFileName(const std::string& prefix) 
{
    // 1. 获取当前时间戳（精确到毫秒）
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();  // 获取自纪元以来的毫秒数

    // 2. 生成一个随机数（4位数字）
    std::random_device rd;            // 获取随机设备
    std::mt19937 gen(rd());           // 使用梅森旋转算法生成随机数引擎
    std::uniform_int_distribution<> dis(1000, 9999);  // 随机数范围从 1000 到 9999

    // 3. 拼接前缀、时间戳和随机数生成文件名
    std::ostringstream fileName;
    fileName << prefix << "_" << timestamp << "_" << dis(gen);  // 生成文件名字符串

    return fileName.str();  // 返回生成的唯一文件名
}

// 获取文件类型，根据文件扩展名返回对应类型
static std::string GetFileType(const std::string& fileName) 
{
    // 1. 查找文件名中最后一个点的位置，获取文件扩展名
    size_t dotPos = fileName.find_last_of('.');
    if (dotPos != std::string::npos && dotPos < fileName.length() - 1) 
    {
        std::string extension = fileName.substr(dotPos + 1);  // 获取文件扩展名
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);  // 转换为小写

        // 2. 根据文件扩展名返回文件类型
        if (extension == "jpg" || extension == "jpeg" || extension == "png" || extension == "gif") 
        {
            return "image";  // 图片类型
        } 
        else if (extension == "mp4" || extension == "avi" || extension == "mov" || extension == "wmv") 
        {
            return "video";  // 视频类型
        } 
        else if (extension == "pdf") 
        {
            return "pdf";    // PDF 文件
        } 
        else if (extension == "doc" || extension == "docx") 
        {
            return "word";   // Word 文件
        } 
        else if (extension == "xls" || extension == "xlsx") 
        {
            return "excel";  // Excel 文件
        } 
        else if (extension == "ppt" || extension == "pptx") 
        {
            return "powerpoint";  // PowerPoint 文件
        } 
        else if (extension == "txt" || extension == "csv") 
        {
            return "text";  // 文本文件
        } 
        else 
        {
            return "other";  // 其他文件类型
        }
    }
    return "unknown";  // 无效文件扩展名
}

// 转义 SQL 字符串，防止 SQL 注入攻击
static std::string EscapeString(const std::string& str, MYSQL* mysql) 
{
    // 1. 如果 MySQL 未初始化，直接返回原始字符串
    if (!mysql) 
    {
        return str;
    }

    // 2. 分配足够的空间来存放转义后的字符串
    char* escaped = new char[str.length() * 2 + 1];  // 估算最大大小
    
    mysql_real_escape_string(mysql, escaped, str.c_str(), str.length());  // 执行转义操作

    // 3. 将转义后的字符数组转换为 std::string 并返回
    std::string result(escaped);
    delete[] escaped;  // 释放分配的内存

    return result;
}

static std::string sha256(const std::string& input) 
{
    // 使用 std::hash 生成 size_t 类型的哈希值（非加密安全）
    std::hash<std::string> hasher;
    auto hash = hasher(input);
    
    // 将 hash 转为十六进制字符串表示
    std::stringstream ss;
    ss << std::hex << hash;
    return ss.str();
}

// 生成 Argon2 编码后的哈希（含参数和盐）
// static std::string hash_password_argon2(const std::string& password, const std::string& salt) 
// {
//     char encoded[128];      // 编码后输出
//     char raw_hash[32];      // 原始哈希（一般不使用）

//     int result = argon2id_hash_encoded(
//         2,                  // 时间成本
//         1 << 16,            // 内存成本 64MB
//         1,                  // 并行度
//         password.c_str(),
//         password.length(),
//         salt.c_str(),
//         salt.length(),
//         raw_hash,
//         sizeof(raw_hash),
//         encoded,
//         sizeof(encoded)
//     );

//     if (result != ARGON2_OK) 
//     {
//         std::cerr << "Argon2 hashing failed: " << argon2_error_message(result) << std::endl;
//         return "";
//     }

//     return std::string(encoded);
// }

// 生成会话id
static std::string GenerateSessionId() 
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 35);
    
    const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string sessionId;
    sessionId.reserve(32);
    
    for (int i = 0; i < 32; ++i) {
        sessionId += chars[dis(gen)];
    }
    
    return sessionId;
}

// 生成分享码
static std::string GenerateShareCode() 
{
    static const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 35);

    std::string code;
    code.reserve(32);
    for (int i = 0; i < 32; ++i) 
    {
        code += chars[dis(gen)];
    }
    return code;
}

// 生成提取码
static std::string GenerateExtractCode() 
{
    static const char* chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 35);

    std::string code;
    code.reserve(6);
    for (int i = 0; i < 6; ++i) 
    {
        code += chars[dis(gen)];
    }
    return code;
}

#endif