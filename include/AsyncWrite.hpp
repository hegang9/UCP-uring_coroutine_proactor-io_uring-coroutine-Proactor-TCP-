#pragma once
#include <coroutine>
#include <cstddef>

#include "MemoryPool.hpp"

class TcpConnection;

class AsyncWriteAwaitable
{
  public:
    // 禁用拷贝和赋值
    AsyncWriteAwaitable(const AsyncWriteAwaitable &) = delete;
    AsyncWriteAwaitable &operator=(const AsyncWriteAwaitable &) = delete;
    // 普通写操作，从 outputBuffer_ 发送数据
    AsyncWriteAwaitable(TcpConnection *conn)
        : conn_(conn), regBuf_(nullptr), regBufLen_(0), regBufIdx_(-1), inFd_(-1), offset_(0), count_(0), isZc_(false)
    {
    }
    // 固定缓冲区写操作，直接从已注册缓冲区发送数据
    AsyncWriteAwaitable(TcpConnection *conn, void *regBuf, size_t len, int idx)
        : conn_(conn), regBuf_(regBuf), regBufLen_(len), regBufIdx_(idx), inFd_(-1), offset_(0), count_(0), isZc_(false)
    {
    }
    // Sendfile 零拷贝操作
    AsyncWriteAwaitable(TcpConnection *conn, int inFd, off_t offset, size_t count, bool isZc)
        : conn_(conn), regBuf_(nullptr), regBufLen_(0), regBufIdx_(-1), inFd_(inFd), offset_(offset), count_(count),
          isZc_(isZc)
    {
    }
    // 用户游离缓冲区的SEND_ZC零拷贝写操作
    AsyncWriteAwaitable(TcpConnection *conn, const char *data, size_t len, bool isZc)
        : conn_(conn), regBuf_(const_cast<char *>(data)), regBufLen_(len), regBufIdx_(-1), inFd_(-1), offset_(0),
          count_(0), isZc_(isZc)
    {
    }
    // 固定缓冲区的SEND_ZC零拷贝写操作
    AsyncWriteAwaitable(TcpConnection *conn, void *regBuf, size_t len, int idx, bool isZc)
        : conn_(conn), regBuf_(regBuf), regBufLen_(len), regBufIdx_(idx), inFd_(-1), offset_(0), count_(0), isZc_(isZc)
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept;
    int await_resume() const noexcept;
    ~AsyncWriteAwaitable() = default;

    // 重载new/delete，接入内存池
    static void *operator new(size_t size)
    {
        return HashBucket::useMemory(size);
    }
    static void operator delete(void *p, size_t size)
    {
        HashBucket::freeMemory(p, size);
    }

  private:
    TcpConnection *conn_; // 关联的TcpConnection对象
    void *regBuf_;        // 已注册缓冲区指针
    size_t regBufLen_;    // 已注册缓冲区数据长度
    int regBufIdx_;       // 已注册缓冲区索引（用于写完后归还）

    int inFd_;     // 输入文件描述符（sendfile模式）
    off_t offset_; // 文件偏移量（sendfile模式）
    size_t count_; // 发送字节数（sendfile模式）

    // 背压机制相关状态（仅在 kBlock 策略下生效）
    int totalWritten_ = 0;   // 记录在阻塞期间，底层 io_uring 累计成功写入的字节数
    bool isBlocked_ = false; // 标记当前协程是否因为触发高水位而进入了阻塞挂起模式

    // 零拷贝相关状态
    bool isZc_ = false; // 标记是否是零拷贝发送（用户缓冲区或 sendfile）
};