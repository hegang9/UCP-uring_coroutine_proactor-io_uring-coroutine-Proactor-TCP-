#include "AsyncWrite.hpp"

#include <iostream>
#include <thread>

#include "Buffer.hpp"
#include "TcpConnection.hpp"

void AsyncWriteAwaitable::await_suspend(std::coroutine_handle<> handle) noexcept
{
    auto &ctx = conn_->getWriteContext();

    // 背压机制：检查是否需要触发 kBlock 阻塞策略（仅对普通模式生效，固定缓冲区模式不经过 outputBuffer_）
    // 触发条件：未开启固定缓冲区 && 配置了 kBlock 策略 && 发送缓冲区数据量已达到或超过高水位阈值
    if (regBuf_ == nullptr && conn_->getBackpressureConfig().strategy == BackpressureStrategy::kBlock &&
        conn_->getOutputBuffer().readableBytes() >= conn_->getBackpressureConfig().outputBufferHighWaterMark)
    {

        isBlocked_ = true;
        // 关键点：将 coro_handle 置空，这样 io_uring 完成事件到来时，不会直接唤醒协程
        // 而是转而执行下面设置的 handler 回调，从而实现协程的“挂起”
        ctx.coro_handle = nullptr;

        // 设置一个循环处理的 handler，由底层 EventLoop 在每次 io_uring 写完后调用
        ctx.handler = [this, handle](int res) {
            auto &writeCtx = conn_->getWriteContext();

            if (res > 0)
            {
                // 每次成功写入一部分数据，就从缓冲区中移除，并累加已写字节数
                conn_->getOutputBuffer().retrieve(res);
                totalWritten_ += res;
            }

            // 检查解除阻塞的条件：
            // 1. 发生错误或连接断开 (res <= 0)
            // 2. 缓冲区积压的数据已经被发送出去，剩余数据量降至低水位阈值以下
            if (res <= 0 ||
                conn_->getOutputBuffer().readableBytes() <= conn_->getBackpressureConfig().outputBufferLowWaterMark)
            {
                // 满足条件，准备唤醒协程
                // 将最终的写入结果（累计写入量或错误码）存入上下文，供 await_resume 读取
                writeCtx.result_ = (res <= 0 && totalWritten_ == 0) ? res : totalWritten_;

                // 注意：绝对不能在这里执行 writeCtx.handler = nullptr;
                // 因为当前代码正运行在这个 lambda 内部，清除 handler 会导致 lambda 自身被析构，引发未定义行为(UB)
                // handler 的安全清除工作必须推迟到协程恢复后的 await_resume 中进行
                handle.resume();
            }
            else
            {
                // 缓冲区数据量仍然高于低水位，继续向 io_uring 提交写请求，协程继续保持挂起状态
                conn_->submitWriteRequest();
            }
        };

        // 提交第一次写请求，启动 handler 循环
        conn_->submitWriteRequest();
    }
    else
    {
        // 正常模式（未触发背压）：将协程句柄保存到 IoContext 中
        // 当 io_uring 写操作完成时，EventLoop 会直接调用 handle.resume() 唤醒协程
        ctx.coro_handle = handle;
        ctx.handler = nullptr;

        if (inFd_ >= 0)
        {
            // Sendfile 零拷贝模式
            conn_->incrementPendingSpecialWrite();
            conn_->submitSendfileRequest(inFd_, offset_, count_);
        }
        else if (regBuf_ != nullptr)
        {
            // 固定缓冲区模式：使用已注册缓冲区发送数据
            conn_->incrementPendingSpecialWrite();
            conn_->submitWriteRequestWithRegBuffer(regBuf_, regBufLen_, regBufIdx_);
        }
        else
        {
            // 普通模式：从 outputBuffer_ 发送数据
            conn_->submitWriteRequest();
        }
    }
}

int AsyncWriteAwaitable::await_resume() const noexcept
{
    auto &ctx = conn_->getWriteContext();
    // 读取实际写入的字节数（或错误码），在后续开发中业务层可根据此结果进行错误处理
    int n = ctx.result_;

    // 安全清除 handler：此时协程已经恢复，离开 lambda 作用域，可以安全地销毁 handler
    ctx.handler = nullptr;

    if (inFd_ >= 0)
    {
        // Sendfile 零拷贝模式
        conn_->decrementPendingSpecialWrite();
    }
    else if (regBuf_ != nullptr)
    {
        // 固定缓冲区模式
        conn_->decrementPendingSpecialWrite();
    }
    else if (!isBlocked_ && n > 0)
    {
        // 正常模式
        conn_->getOutputBuffer().retrieve(n);
    }

    // 每次实际的写操作完成后，检查连接是否处于 kDisconnecting 断开中状态。
    // 如果是，并且 outputBuffer_ 已经被清空，说明收尾工作执行完毕，真正调用物理 shutdown
    conn_->maybeShutdownWrite();

    return n;
}