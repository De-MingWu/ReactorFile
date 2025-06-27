#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <sys/stat.h>   // 包含获取文件信息的系统调用
#include <fcntl.h>      // 包含文件控制操作的标头
#include <unistd.h>     // 包含 POSIX 系统调用，如 read, close, lseek
#include <cstdint>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem; 

// 文件下载上下文类
class FileDownContext 
{
private:
    std::string filepath_;        // 文件路径
    std::string originalFileName_; // 原始文件名
    std::ifstream file_;          // 输入文件流描述符
    uintmax_t fileSize_;          // 文件总大小
    uintmax_t currentPosition_;   // 当前读取位置
    bool isComplete_;             // 文件是否读取完成

public:
    FileDownContext(const std::string& filepath, const std::string& originalFileName);
    ~FileDownContext();

    // 将文件指针移至指定位置
    void SeekTo(uintmax_t position);

    // 读取下一个文件块
    bool ReadNextChunk(std::string& chunk);

    // 检查文件是否读取完成
    bool IsComplete() const;

    // 获取当前读取位置
    uintmax_t GetCurrentPosition() const;

    // 获取文件总大小
    uintmax_t GetFileSize() const;

    // 获取原始文件名
    const std::string& GetOriginalFileName() const;
};
