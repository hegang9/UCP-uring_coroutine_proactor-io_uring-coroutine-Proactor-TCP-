#include "TcpConnection.hpp"

#include <unistd.h>

#include <cstring>

#include "Logger.hpp"

TcpConnection::TcpConnection(const std::string &name, EventLoop *loop, int sockfd, const InetAddress &peerAddr)
    : name_(name), loop_(loop), socket_(sockfd), state_(TcpConnectionState::kConnecting), reading_(false),
      curReadBuffer_(nullptr), curReadBufferSize_(0), curReadBufferOffset_(0), outputBuffer_(),
      readContext_(IoType::Read, sockfd), writeContext_(IoType::Write, sockfd),
      timeoutContext_(IoType::Timeout, sockfd), readTimeout_(0), readTimeoutSpec_(),
      localAddr_(socket_.getLocalAddress()), peerAddr_(peerAddr), connectionCallback_(nullptr), closeCallback_(nullptr)
{
    // 协程模式下，不需要绑定传统的回调函数 (handleRead/handleWrite)
}

TcpConnection::~TcpConnection()
{
    // 逻辑关闭连接，调用socket的析构函数释放资源
}

void TcpConnection::setState(TcpConnectionState state)
{
    state_.store(state);
}

void TcpConnection::reset()
{
    socket_.reset();
    state_.store(TcpConnectionState::kDisconnected);
    closeCallbackInvoked_.store(false);
    reading_ = false;
    outputBuffer_.reset();
    // 读写上下文不需要重置fd，因为TcpConnection对象销毁时，fd已经关闭
    readContext_.coro_handle = nullptr;
    readContext_.result_ = 0;
    if (readContext_.idx >= 0)
    {
        loop_->returnRegisteredBuffer(readContext_.idx);
    }
    readContext_.idx = -1; // 重置 registered buffer 索引
    curReadBuffer_ = nullptr;
    curReadBufferSize_ = 0;
    curReadBufferOffset_ = 0;
    writeContext_.coro_handle = nullptr;
    writeContext_.result_ = 0;
    loop_ = nullptr;
}

void TcpConnection::shutdown()
{
    // 发送FIN包，半关闭写端
    if (state_.load() == TcpConnectionState::kConnected)
    {
        setState(TcpConnectionState::kDisconnecting);
        socket_.shutdownWrite();
    }
}

void TcpConnection::forceClose()
{
    TcpConnectionState expected = TcpConnectionState::kConnected;
    if (state_.compare_exchange_strong(expected, TcpConnectionState::kDisconnecting))
    {
        LOG_INFO("TcpConnection::forceClose CAS success, queueing handleClose, conn={}", name_);
        loop_->queueInLoop(std::bind(&TcpConnection::handleClose, shared_from_this()));
    }
    else
    {
        LOG_INFO("TcpConnection::forceClose CAS failed, state={}, conn={}", static_cast<int>(expected), name_);
    }
}

void TcpConnection::submitReadRequest(size_t nbytes)
{
    if (!isConnected())
    {
        LOG_WARN("TcpConnection::submitReadRequest: state not connected, name={}", name_);
        return;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop_->ring_);
    if (!sqe)
    {
        // 极其罕见的情况：SQ 满了。
        // 实际生产中可能需要处理，这里简单打印
        LOG_ERROR("TcpConnection::submitReadRequest: SQ full");
        return;
    }
    int idx = loop_->getRegisteredBufferIndex();
    if (idx >= 0)
    {
        // 使用已注册缓冲区进行读操作
        void *buf = loop_->getRegisteredBuffer(idx);
        // 把已注册缓冲区的索引存到 IoContext 的 idx 字段，以便完成后归还
        readContext_.idx = idx;
        io_uring_prep_read_fixed(sqe, socket_.getFd(), buf, nbytes, 0, idx);
        io_uring_sqe_set_data(sqe, &readContext_);
    }
    else
    {
        // 输出错误信息
        LOG_ERROR("TcpConnection::submitReadRequest: no registered buffer available");
    }

    if (readTimeout_ > std::chrono::milliseconds::zero())
    {
        io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
        io_uring_sqe *ts_sqe = io_uring_get_sqe(&loop_->ring_);
        if (ts_sqe)
        {
            io_uring_prep_link_timeout(ts_sqe, &readTimeoutSpec_, 0);
            io_uring_sqe_set_data(ts_sqe, &timeoutContext_);
        }
        else
        {
            LOG_ERROR("TcpConnection::submitReadRequest: link timeout sqe unavailable");
        }
    }
}

void TcpConnection::submitReadRequestWithUserBuffer(char *userBuf, size_t userBufCap, size_t nbytes)
{
    if (!isConnected())
    {
        LOG_WARN("TcpConnection::submitReadRequestWithUserBuffer: state not connected, name={}", name_);
        return;
    }
    if (userBuf == nullptr || userBufCap == 0)
    {
        // 无效的用户缓冲区，直接返回
        LOG_ERROR("TcpConnection::submitReadRequestWithUserBuffer: invalid user buffer");
        return;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop_->ring_);
    if (!sqe)
    {
        // 极其罕见的情况：SQ 满了。
        // 实际生产中可能需要处理，这里简单打印
        LOG_ERROR("TcpConnection::submitReadRequestWithUserBuffer: SQ full");
        return;
    }
    // 使用用户提供的缓冲区进行读操作
    io_uring_prep_read(sqe, socket_.getFd(), userBuf, std::min(userBufCap, nbytes), 0);
    io_uring_sqe_set_data(sqe, &readContext_);
    // 标记 idx 为 -1，表示未使用已注册缓冲区
    readContext_.idx = -1;

    if (readTimeout_ > std::chrono::milliseconds::zero())
    {
        io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
        io_uring_sqe *ts_sqe = io_uring_get_sqe(&loop_->ring_);
        if (ts_sqe)
        {
            io_uring_prep_link_timeout(ts_sqe, &readTimeoutSpec_, 0);
            io_uring_sqe_set_data(ts_sqe, &timeoutContext_);
        }
        else
        {
            LOG_ERROR("TcpConnection::submitReadRequestWithUserBuffer: link timeout sqe unavailable");
        }
    }
}

void TcpConnection::submitWriteRequest()
{
    if (!isConnected() && !isDisconnecting())
    {
        LOG_WARN("TcpConnection::submitWriteRequest: invalid state, name={}", name_);
        return;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop_->ring_);
    if (!sqe)
    {
        // 极其罕见的情况：SQ 满了。
        // 实际生产中可能需要处理，这里简单打印
        LOG_ERROR("TcpConnection::submitWriteRequest: SQ full");
        return;
    }

    // 准备写操作
    // 注意：write 操作不应该修改 outputBuffer_
    // 的可读位置，直到写操作完成(handleWrite)
    io_uring_prep_write(sqe, socket_.getFd(), outputBuffer_.readBeginAddr(), outputBuffer_.readableBytes(), 0);
    io_uring_sqe_set_data(sqe, &writeContext_);
    // 标记未使用已注册缓冲区
    writeContext_.idx = -1;
}

void TcpConnection::submitWriteRequestWithRegBuffer(void *buf, size_t len, int idx)
{
    if (!isConnected() && !isDisconnecting())
    {
        LOG_WARN("TcpConnection::submitWriteRequestWithRegBuffer: invalid state, name={}", name_);
        return;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop_->ring_);
    if (!sqe)
    {
        LOG_ERROR("TcpConnection::submitWriteRequestWithRegBuffer: SQ full");
        return;
    }

    // 使用已注册缓冲区进行零拷贝写操作
    io_uring_prep_write_fixed(sqe, socket_.getFd(), buf, len, 0, idx);
    io_uring_sqe_set_data(sqe, &writeContext_);
    // 记录已注册缓冲区索引，写完后由调用者归还
    writeContext_.idx = idx;
}

void TcpConnection::setTimeout(std::chrono::milliseconds timeout)
{
    readTimeout_ = timeout;
    readTimeoutSpec_.tv_sec = timeout.count() / 1000;
    readTimeoutSpec_.tv_nsec = (timeout.count() % 1000) * 1000000;
}

void TcpConnection::handleClose()
{
    LOG_INFO("TcpConnection::handleClose called, conn={}, state={}", name_, static_cast<int>(state_.load()));
    TcpConnectionState state = state_.load();
    if (state == TcpConnectionState::kDisconnected)
    {
        LOG_INFO("TcpConnection::handleClose already disconnected, conn={}", name_);
        return;
    }
    state_.store(TcpConnectionState::kDisconnecting);
    if (closeCallbackInvoked_.exchange(true))
    {
        LOG_INFO("TcpConnection::handleClose closeCallback already invoked, conn={}", name_);
        return;
    }
    // 保护 TcpConnection，防止在回调过程中被销毁
    std::shared_ptr<TcpConnection> guard(shared_from_this());
    if (closeCallback_)
    {
        LOG_INFO("TcpConnection::handleClose calling closeCallback, conn={}", name_);
        closeCallback_(guard);
    }
    else
    {
        LOG_WARN("TcpConnection::handleClose closeCallback is null, conn={}", name_);
    }
}

void TcpConnection::releaseCurReadBuffer()
{
    if (readContext_.idx >= 0)
    {
        loop_->returnRegisteredBuffer(readContext_.idx);
        readContext_.idx = -1; // 重置 idx，防止重复归还
    }
    curReadBuffer_ = nullptr;
    curReadBufferSize_ = 0;
    curReadBufferOffset_ = 0;
}

void TcpConnection::connectEstablished()
{
    // 将状态设置为已连接
    setState(TcpConnectionState::kConnected);

    // 初始化 IoContext 的弱引用，用于 Cancel CQE 安全检查
    readContext_.connection = shared_from_this();
    writeContext_.connection = shared_from_this();
    timeoutContext_.connection = shared_from_this();
    // 修复循环引用：使用 weak_ptr 而不是直接捕获 shared_ptr
    timeoutContext_.handler = [weak_self = std::weak_ptr<TcpConnection>(shared_from_this())](int res) {
        LOG_INFO("Timeout handler called, res={}", res);
        if (res == -ECANCELED) // 返回-ECANCELED表示没有超时，直接返回
            return;

        // 尝试提升 weak_ptr
        auto self = weak_self.lock();
        if (!self)
        {
            LOG_INFO("Timeout handler: connection already destroyed");
            return;
        }
        if (!self->isConnected())
        {
            LOG_INFO("Timeout handler: connection not connected");
            return;
        }

        LOG_INFO("Connection {} timed out, forcing close", self->getName());
        self->forceClose(); // 说明发生超时，强制关闭连接
    };

    // 这里调用 connectionCallback_
    if (connectionCallback_)
    {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed()
{
    LOG_INFO("TcpConnection::connectDestroyed called, conn={}, state={}, fd={}", name_, static_cast<int>(state_.load()),
             socket_.getFd());
    if (state_ == TcpConnectionState::kConnected || state_ == TcpConnectionState::kDisconnecting)
    {
        setState(TcpConnectionState::kDisconnected);
    }
    closeCallbackInvoked_.store(true);

    // 关键修复：主动关闭底层 Socket 文件描述符
    // 否则如果还有其他地方（比如 io_uring 的 IoContext）持有 shared_ptr，
    // Socket 的析构函数就不会被调用，fd 就不会被关闭，连接也就一直挂着。
    socket_.closeFd();
    LOG_INFO("TcpConnection::connectDestroyed fd closed, conn={}", name_);

    // io_uring 中挂起的请求会因为 fd 关闭而以 -ECANCELED 或 -EBADF 失败。

    // 这里只设置连接状态，是因为TcpConnection对象是使用shared_ptr管理的，当没有引用时会自动销毁
}

// 协程模式下，这些回调如果不使用，可以留空。
// 提供实现以避免链接错误。
// void TcpConnection::handleRead(int) {}
// void TcpConnection::handleWrite(int) {}
// 检查发送缓冲区是否触发背压（在每次 asyncSend 追加数据前调用）
// 目的：防止慢接收客户端导致服务端发送缓冲区无限膨胀，最终 OOM
void TcpConnection::checkOutputBufferBackpressure(size_t incomingBytes)
{
    size_t currentSize = outputBuffer_.readableBytes();
    size_t newSize = currentSize + incomingBytes;

    // 统计：记录缓冲区达到的最大峰值，便于后续调优
    outputBufferStats_.maxSize = std::max(outputBufferStats_.maxSize, newSize);

    // 检查是否超过高水位阈值
    if (newSize >= backpressureConfig_.outputBufferHighWaterMark)
    {
        // 使用原子操作确保只在第一次跨越水位时打印日志和计数
        bool wasInHighWaterMark = inHighWaterMark_.exchange(true);
        if (!wasInHighWaterMark)
        {
            outputBufferStats_.highWaterMarkCount++;
            LOG_WARN("TcpConnection: output buffer high water mark reached, conn={}, bufSize={}, threshold={}", name_,
                     newSize, backpressureConfig_.outputBufferHighWaterMark);
        }

        // 根据配置的策略执行相应的背压动作
        switch (backpressureConfig_.strategy)
        {
        case BackpressureStrategy::kDiscard:
            LOG_ERROR("TcpConnection: discarding data due to backpressure, conn={}, discardBytes={}", name_,
                      incomingBytes);
            outputBufferStats_.discardedBytes += incomingBytes;
            // 不 append，直接返回（数据被丢弃）
            return;

        case BackpressureStrategy::kCloseConnection:
            LOG_ERROR("TcpConnection: closing connection due to backpressure, conn={}, bufSize={}", name_, newSize);
            forceClose();
            return;

        case BackpressureStrategy::kPass:
            // 继续发送，不做任何处理
            break;

        case BackpressureStrategy::kBlock:
            // 阻塞策略：在 AsyncWriteAwaitable 中实现协程挂起，直到水位降至低水位
            LOG_WARN("TcpConnection: output buffer high water mark reached, blocking coroutine, conn={}", name_);
            break;
        }
    }
    else if (newSize <= backpressureConfig_.outputBufferLowWaterMark && inHighWaterMark_.load())
    {
        // 从高水位恢复到正常
        inHighWaterMark_.store(false);
        LOG_WARN("TcpConnection: output buffer low water mark restored, conn={}, bufSize={}, threshold={}", name_,
                 newSize, backpressureConfig_.outputBufferLowWaterMark);
    }
}
