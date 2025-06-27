#include "FileDownContext.h"
#include "Log.h"

// 构造函数，初始化下载上下文
FileDownContext::FileDownContext(const std::string& filepath, const std::string& originalFileName)
                : filepath_(filepath),
                  originalFileName_(originalFileName), // 原始文件名
                  fileSize_(0),                        // 文件大小，初始化为0
                  currentPosition_(0),                 // 当前读取位置，初始化为0
                  isComplete_(false)                   // 是否完成，初始化为false
{
    // 获取文件大小
    fileSize_ = fs::file_size(filepath_);
    
    // 打开文件
    file_.open(filepath_, std::ios::binary | std::ios::in);
    if (!file_.is_open()) 
    {
        LOG_ERROR << "Failed to open file: " << filepath_;
        throw std::runtime_error("Failed to open file: " + filepath_);
    }
    LOG_INFO << "Opening file for download: " << filepath_ << ", size: " << fileSize_;
}

// 析构函数，关闭文件
FileDownContext::~FileDownContext() 
{
    if (file_.is_open()) file_.close();
}

// 将文件指针移至指定位置
void FileDownContext::SeekTo(uintmax_t position) 
{
     // 如果文件未成功打开，抛出异常
    if (!file_.is_open()) 
    {
        throw std::runtime_error("File is not open: " + filepath_);
    }
    file_.seekg(position);  // 移动文件指针
    currentPosition_ = position;// 更新当前读取位置
    isComplete_ = false;// 重置完成标志
}

// 读取下一个文件块
bool FileDownContext::ReadNextChunk(std::string& chunk) 
{
    // 如果文件未打开或已经完成，返回 false
    if (!file_.is_open() || isComplete_) return false;

    const uintmax_t chunkSize = 1024 * 1024; // 1MB 每次读取的数据块大小
    uintmax_t remainingBytes = fileSize_ - currentPosition_; // 剩余未读取的字节数
    uintmax_t bytesToRead = std::min(chunkSize, remainingBytes); // 本次要读取的字节数

    if (bytesToRead == 0) 
    {
        // 如果没有数据可读取，标记为完成并返回 false
        isComplete_ = true;
        return false;
    }

    std::vector<char> buffer(bytesToRead); // 缓冲区，用于存储读取的数据
    file_.read(buffer.data(), bytesToRead);// 从文件读取数据
    chunk.assign(buffer.data(), bytesToRead);// 将读取到的内容存入 chunk
    currentPosition_ += bytesToRead;// 更新当前读取位置

    // 记录日志，显示已读取的字节数和当前位置
    LOG_INFO << "Read chunk of " << bytesToRead << " bytes, current position: " 
                << currentPosition_ << "/" << fileSize_;
    return true;
}

// 检查文件是否读取完成
bool FileDownContext::IsComplete() const { return isComplete_; }

// 获取当前读取位置
uintmax_t FileDownContext::GetCurrentPosition() const { return currentPosition_; }

// 获取文件总大小
uintmax_t FileDownContext::GetFileSize() const { return fileSize_; }

// 获取原始文件名
const std::string& FileDownContext::GetOriginalFileName() const { return originalFileName_; }