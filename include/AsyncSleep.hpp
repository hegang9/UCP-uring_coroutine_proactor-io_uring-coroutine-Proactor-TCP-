#pragma once
#include <chrono>
#include <coroutine>
#include <liburing.h>

#include "EventLoop.hpp"
#include "IoContext.hpp"
#include "MemoryPool.hpp"

class AsyncSleepAwaitable
{
  public:
    template <typename Rep, typename Period>
    AsyncSleepAwaitable(EventLoop *loop, std::chrono::duration<Rep, Period> duration)
        : loop_(loop), timeoutContext_(IoType::Timeout, -1) // 超时事件不关联文件描述符，使用-1占位
    {
        // 将chrono转换为io_uring所需的__kernel_timespec 结构时间格式.
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - secs);

        ts_.tv_sec = secs.count();
        ts_.tv_nsec = nsecs.count();
    }

    bool await_ready() const noexcept
    {
        return false; // 总是异步等待
    }

    // 协程挂起时提交超时任务给io_uring
    void await_suspend(std::coroutine_handle<> handle);

    void await_resume() const noexcept
    {
        // 超时事件完成后恢复协程，无需返回值
    }

    ~AsyncSleepAwaitable() = default;

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
    EventLoop *loop_;             // 关联的事件循环，用于提交超时任务
    struct __kernel_timespec ts_; // io_uring使用的时间格式
    IoContext timeoutContext_;    // 超时事件的上下文
};