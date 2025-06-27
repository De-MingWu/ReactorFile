#include <string>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <iostream>
#include <cstdint>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem; 

// 上传解析状态枚举
enum class State 
{
    kExpectHeaders,    // 等待解析 multipart 请求头（Content-Disposition、Content-Type 等）
    kExpectContent,    // 进入文件内容写入阶段
    kExpectBoundary,   // 等待下一个 boundary，判断是否还有下一个文件块
    kComplete          // 文件上传已完成
};

// 文件上传上下文类，用于管理上传过程中单个文件的写入状态、边界等信息
class FileUploadContext 
{
private:
    std::string fileName_;         // 文件在服务器上的保存路径
    std::string originalFileName_; // 上传文件的原始名称（如上传表单中文件名）
    std::ofstream file_;           // 文件输出流，用于写入上传内容
    uintmax_t totalBytes_;         // 累计写入的文件字节数
    State state_;                  // 上传状态
    std::string boundary_;         // multipart/form-data 边界字符串


public:
    // 构造函数：传入保存路径和原始文件名
    FileUploadContext(const std::string& fileName, const std::string& originalFileName);

    // 析构函数：关闭文件句柄
    ~FileUploadContext();

    // 写入数据到文件
    void WriteData(const char* data, size_t len);

    // 获取已写入总字节数
    uintmax_t GetTotalBytes() const;

    // 获取服务器保存路径
    const std::string& GetFileName() const;

    // 获取客户端上传的原始文件名
    const std::string& GetOriginalFileName() const;

    // 设置 multipart 边界字符串
    void SetBoundary(const std::string& boundary);
    // 获取 multipart 边界
    const std::string& GetBoundary()const;

    // 获取当前解析状态
    State GetState() const;
    // 设置当前解析状态
    void SetState(State state);
};