#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "HttpServer.h"
#include "AsyncLog.h"
#include "Log.h"

HttpServer *httpServer;

// 设置异步日志
std::unique_ptr<AsyncLog> asynclog(new AsyncLog("../LogFiles/"));

void AsyncOutputFunc(const char *data, std::size_t len)
{
    asynclog->Append(data, len);
}

void AsyncFlushFunc() 
{
    asynclog->Flush();
}

/**
 * 信号2和15处理函数，功能是停止服务程序
 * @param sig
 */
void StopSignal(int sig)
{
    //printf("sig = %d\n", sig);
    
    // 删除 httpServer 前，先停止服务
    httpServer->StopService(); 
    //printf("httpserver已停止。\n");

    // 确保服务停止后再删除 httpServer
    delete httpServer;
    //printf("delete httpServer\n");

    // 停止日志系统
    asynclog->Stop();
    //printf("asynclog->Stop()\n");
    // 退出程序
    exit(0);
    //printf("exit(0)\n");
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("usage: ./httpservice <ip> <port>\n");
        return -1;
    }

    // 设置信号处理函数
    signal(SIGTERM, StopSignal);
    signal(SIGINT, StopSignal);

    // 设置异步日志输出和刷新
    Log::SetOutput(AsyncOutputFunc);
    Log::SetFlush(AsyncFlushFunc);

    asynclog->Start();

    // 创建 HttpServer 并绑定日志系统
    httpServer = new HttpServer(argv[1], 
                                atoi(argv[2]), 
                                3, // 从事件线程
                                0, // 工作事件线程
                                "./uploads", // 上传文件二进制存储位置
                                "uploads/filename_mapping.json" ); // 映射文件位置

    // 事件循环
    httpServer->Start();

    return 0;
}

