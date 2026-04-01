#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include "AsyncRead.hpp"
#include "AsyncWrite.hpp"
#include "Buffer.hpp"
#include "CoroutineTask.hpp"
#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include "IoContext.hpp"
#include "Logger.hpp"
#include "MemoryPool.hpp"
#include "Socket.hpp"

// TCP连接状态枚举
enum class TcpConnectionState
{
    kDisconnected, // 断开连接
    kConnecting,   // 连接中
    kConnected,    // 已连接
    kDisconnecting // 断开中
};

// 背压策略枚举
// 连接级别的背压策略：当发送缓冲区(outputBuffer_)达到高水位时采取的动作
enum class BackpressureStrategy
{
    kDiscard,         // 丢弃新数据：保护内存，但会导致数据丢失（适用于允许丢包的场景）
    kCloseConnection, // 断开连接：极端保护措施，防止恶意客户端慢接收导致服务端 OOM
    kBlock,           // 阻塞等待：最优雅的策略，挂起当前发送协程，直到缓冲区降至低水位再恢复发送
    kPass             // 不处理：继续追加数据，可能导致内存无限增长（需业务层自行控制）
};

class TcpConnection : public std::enable_shared_from_this<TcpConnection>
{
  public:
    // 连接建立/断开回调
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection> &)>;
    // 消息到来回调
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection> &)>;

    TcpConnection(const std::string &name, EventLoop *loop, int sockfd, const InetAddress &peerAddr);
    ~TcpConnection();

    // 禁用拷贝和赋值
    TcpConnection(const TcpConnection &) = delete;
    TcpConnection &operator=(const TcpConnection &) = delete;

    // 设置回调函数
    void setConnectionCallback(const ConnectionCallback &cb)
    {
        connectionCallback_ = cb;
    }
    void setCloseCallback(const CloseCallback &cb)
    {
        closeCallback_ = cb;
    }

    // 改变连接状态
    void setState(TcpConnectionState state);

    bool isConnected() const
    {
        return state_.load() == TcpConnectionState::kConnected;
    }
    bool isDisconnecting() const
    {
        return state_.load() == TcpConnectionState::kDisconnecting;
    }
    // 连接状态检查：在提交读写请求前检查连接状态，避免无效操作
    /**
     * 为什么kDisConnecting状态也算是有效状态？因为在这个状态下，连接虽然正在断开，但还没有完全断开，底层Socket还没有被关闭，所以仍然可以提交写请求来发送剩余数据，或者提交读请求来读取对端发送的最后数据（如ACK）。如果在这个状态下拒绝提交请求，就无法实现优雅的连接关闭了。
     * 当然，如果业务场景不需要在断开过程中继续读写，也可以选择更严格地只允许kConnected状态，这取决于具体需求。
     * 这里的设计是为了提供更大的灵活性，让用户根据自己的业务需求来决定是否允许在断开过程中继续操作。
     * 但无论如何，在提交请求前都应该检查连接状态，避免在完全断开后提交无效请求导致错误。
     *
     */
    bool checkConnected() const
    {
        if (isConnected() || isDisconnecting())
        {
            return true;
        }
        LOG_WARN("TcpConnection::checkConnected: invalid state, name={}, state={}", name_,
                 static_cast<int>(state_.load()));
        return false;
    }

    // 重置TcpConnection，防止复用内存池中的内存时还残留上一个TcpConnection的脏数据
    void reset();

    // 获取所属的 EventLoop
    EventLoop *getLoop() const
    {
        return loop_;
    };

    // 获取本地地址
    InetAddress getLocalAddr() const
    {
        return localAddr_;
    }

    // 获取对端地址
    InetAddress getPeerAddr() const
    {
        return peerAddr_;
    }

    // 获取连接名称
    const std::string &getName() const
    {
        return name_;
    }

    // 发送FIN包，半关闭写端
    void shutdown();
    // 检查并在合适的时机执行半关闭
    void maybeShutdownWrite();
    // 强制关闭连接
    void forceClose();

    // 断开连接处理函数
    void handleClose();

    // 提交异步读写操作到io_uring
    void submitReadRequest(size_t nbytes);
    void submitReadRequestWithUserBuffer(char *userBuf, size_t userBufCap, size_t nbytes);
    void submitWriteRequest();
    void submitWriteRequestWithRegBuffer(void *buf, size_t len, int idx);
    void submitSendfileRequest(int in_fd, off_t offset, size_t count);
    void submitWriteRequestWithZeroCopy(const char *data, size_t len, bool isZc);      // 游离用户缓冲区的零拷贝发送
    void submitWriteRequestWithZeroCopy(void *regBuf, size_t len, int idx, bool isZc); // 已注册缓冲区的零拷贝发送

    // 设置超时时间
    void setTimeout(std::chrono::milliseconds timeout);

    // 异步读写操作的协程接口，创建 Awaitable
    // 对象（这个Awaitable对象包含了读或写操作所需的所有参数），当使用co_await时会触发await_suspend提交io_uring请求
    AsyncReadAwaitable asyncRead(size_t len)
    {
        return AsyncReadAwaitable(this, len);
    };
    AsyncReadAwaitable asyncRead(char *userBuf, size_t userBufCap, size_t len)
    {
        return AsyncReadAwaitable(this, userBuf, userBufCap, len);
    };
    AsyncWriteAwaitable asyncWrite()
    {
        return AsyncWriteAwaitable(this);
    };

    // 发送数据
    AsyncWriteAwaitable asyncSend(const std::string &data)
    {
        checkOutputBufferBackpressure(data.size());
        outputBuffer_.append(data);
        return asyncWrite();
    }

    AsyncWriteAwaitable asyncSend(const char *data, size_t len)
    {
        checkOutputBufferBackpressure(len);
        outputBuffer_.append(data, len);
        return asyncWrite();
    }

    // 固定缓冲区发送：直接从已注册缓冲区发送数据，不经过 outputBuffer_
    // 通常用于 Echo 、高性能网关代理等场景：读到的数据不需要解析
    AsyncWriteAwaitable asyncSendRegisteredBuffer()
    {
        // 使用当前读缓冲区的数据直接发送
        return AsyncWriteAwaitable(this, curReadBuffer_, curReadBufferSize_, readContext_.idx);
    }

    // Sendfile 零拷贝发送：直接将文件内容发送到 socket，不经过用户态缓冲区
    AsyncWriteAwaitable asyncSendfile(int in_fd, off_t offset, size_t count)
    {
        return AsyncWriteAwaitable(this, in_fd, offset, count, true);
    }

    // IORING_OP_SEND_ZC 零拷贝发送：直接从用户态缓冲区发送数据，绕过内核协议栈的内存拷贝
    AsyncWriteAwaitable asyncSendZeroCopy(const char *data, size_t len)
    {
        checkOutputBufferBackpressure(len);
        return AsyncWriteAwaitable(this, const_cast<char *>(data), len, true);
    }

    // 提供获取IoContext的接口
    IoContext &getReadContext()
    {
        return readContext_;
    }
    IoContext &getWriteContext()
    {
        return writeContext_;
    }

    // 提供获取输入缓冲区的接口
    void *&getCurReadBuffer()
    {
        return curReadBuffer_;
    }
    size_t &getCurReadBufferSize()
    {
        return curReadBufferSize_;
    }
    size_t &getCurReadBufferOffset()
    {
        return curReadBufferOffset_;
    }
    void setCurReadBuffer(void *buf)
    {
        curReadBuffer_ = buf;
    }
    void setCurReadBufferSize(size_t size)
    {
        curReadBufferSize_ = size;
    }
    void setCurReadBufferOffset(size_t offset)
    {
        curReadBufferOffset_ = offset;
    }

    // 释放当前读缓冲区（如果使用了已注册缓冲区，则归还）
    void releaseCurReadBuffer();

    // 提供获取输入缓冲区的统一接口（无论是固定缓冲区还是用户缓冲区）
    std::pair<const char *, size_t> getDataFromBuffer() const
    {
        return {static_cast<const char *>(curReadBuffer_), curReadBufferSize_};
    }

    // 提供获取Buffer的接口
    Buffer &getOutputBuffer()
    {
        return outputBuffer_;
    }

    // 背压配置接口
    // 连接级别的背压配置：控制单个连接的发送缓冲区大小
    struct BackpressureConfig
    {
        // 高水位阈值：当 outputBuffer_ 超过此值时，触发背压策略（如挂起协程）
        size_t outputBufferHighWaterMark = 1048576; // 默认 1MB
        // 低水位阈值：当 outputBuffer_ 降至此值以下时，解除背压（如唤醒协程）
        size_t outputBufferLowWaterMark = 262144; // 默认 256KB
        // 默认背压策略：推荐在协程模式下改为 kBlock
        BackpressureStrategy strategy = BackpressureStrategy::kBlock;
    };

    void setBackpressureConfig(const BackpressureConfig &config)
    {
        backpressureConfig_ = config;
    }

    BackpressureConfig getBackpressureConfig() const
    {
        return backpressureConfig_;
    }

    // 获取输出缓冲区统计信息
    struct OutputBufferStats
    {
        size_t maxSize = 0;              // 观测到的最大大小
        uint64_t highWaterMarkCount = 0; // 触发高水位次数
        uint64_t discardedBytes = 0;     // 丢弃的字节数
    };

    OutputBufferStats getOutputBufferStats() const
    {
        return outputBufferStats_;
    }

    void resetOutputBufferStats()
    {
        outputBufferStats_ = OutputBufferStats();
    }

    // 初始化连接（在所属 Loop 执行）
    void connectEstablished();
    // 销毁连接（在所属 Loop 执行）
    void connectDestroyed();

    // 检查当前连接是否有未完成的异步固定缓冲区/Sendfile/Zero-Copy发送请求
    bool hasPendingSpecialWrite() const
    {
        return pendingSpecialWriteCount_.load() > 0;
    }

    void incrementPendingSpecialWrite()
    {
        pendingSpecialWriteCount_.fetch_add(1);
    }

    void decrementPendingSpecialWrite()
    {
        pendingSpecialWriteCount_.fetch_sub(1);
    }

  private:
    // 背压检查：检查 outputBuffer 是否超过高水位，执行相应策略
    void checkOutputBufferBackpressure(size_t incomingBytes);

    EventLoop *loop_;                       // 所属的 子EventLoop
    Socket socket_;                         // 连接的Socket对象
    std::atomic<TcpConnectionState> state_; // 连接状态
    std::string name_;                      // 连接名称

    std::atomic<int> pendingSpecialWriteCount_{0}; // 有多少个特殊写请求(非 outputBuffer_ 的)正在被 io_uring 处理

    std::atomic_bool closeCallbackInvoked_{false}; // 防止关闭回调被重复触发的保护位

    bool reading_; // 是否处于读状态

    IoContext readContext_;                 // 读操作的上下文
    IoContext writeContext_;                // 写操作的上下文
    IoContext timeoutContext_;              // 超时操作的上下文
    std::chrono::milliseconds readTimeout_; // 读超时时间
    __kernel_timespec readTimeoutSpec_;     // 读超时的内核时间结构体

    // 存储当前读操作使用的输入缓冲区信息，可以为固定缓冲区也可以为用户提供的缓冲区
    void *curReadBuffer_;        // 当前读缓冲区指针
    size_t curReadBufferSize_;   // 当前读缓冲区的有效数据大小
    size_t curReadBufferOffset_; // 当前读缓冲区的偏移位置
    Buffer outputBuffer_;        // 发送缓冲区

    // 背压管理
    BackpressureConfig backpressureConfig_;   // 背压配置
    OutputBufferStats outputBufferStats_;     // 统计信息
    std::atomic_bool inHighWaterMark_{false}; // 是否处于高水位

    const InetAddress localAddr_; // 本地地址
    const InetAddress peerAddr_;  // 对端地址，保存下来以免频繁调用

    // 回调函数对象
    ConnectionCallback connectionCallback_;
    CloseCallback closeCallback_;
};