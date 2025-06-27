#include "FileUploadContext.h"
#include "Log.h"

FileUploadContext::FileUploadContext(const std::string& fileName, const std::string& originalFileName)
                : fileName_(fileName), 
                  originalFileName_(originalFileName), 
                  totalBytes_(0),
                  state_(State::kExpectHeaders), 
                  boundary_("")
{
    // 确保目录存在
    fs::path filePath(fileName_);
    fs::path dir = filePath.parent_path();
    if (!dir.empty() && !fs::exists(dir)) {
        fs::create_directories(dir);
    }

    // 打开文件
    file_.open(fileName_, std::ios::binary | std::ios::out);
    if (!file_.is_open()) {
        LOG_ERROR << "Failed to open file: " << fileName;
        throw std::runtime_error("Failed to open file: " + fileName);
    }
    LOG_INFO << "Creating file: " << fileName << ", original name: " << originalFileName;
}

// 析构函数：关闭文件句柄
FileUploadContext::~FileUploadContext() 
{
    if (file_.is_open()) 
    {
        file_.close();
    }
}

// 写入数据到文件
void FileUploadContext::WriteData(const char* data, size_t len) 
{
    if (!file_.is_open()) 
    {
        throw std::runtime_error("File is not open: " + fileName_);
    }

    // 将数据写入文件
    if (!file_.write(data, len)) 
    {
        throw std::runtime_error("Failed to write to file: " + fileName_);
    }

    file_.flush();  // 刷新缓冲区到磁盘
    totalBytes_ += len; // 记录总写入字节数
}

// 获取已写入总字节数
uintmax_t FileUploadContext::GetTotalBytes() const { return totalBytes_; }

// 获取服务器保存路径
const std::string& FileUploadContext::GetFileName() const { return fileName_; }

// 获取客户端上传的原始文件名
const std::string& FileUploadContext::GetOriginalFileName() const { return originalFileName_; }

// 设置 multipart 边界字符串
void FileUploadContext::SetBoundary(const std::string& boundary) { boundary_ = boundary; }
// 获取 multipart 边界
const std::string& FileUploadContext::GetBoundary() const { return boundary_; }

// 获取当前解析状态
State FileUploadContext::GetState() const { return state_; }
// 设置当前解析状态
void FileUploadContext::SetState(State state) { state_ = state; }

