#include "EventLoopThreadPool.hpp"
#include "EventLoop.hpp"
#include "EventLoopThread.hpp"
#include <thread>

#include "Logger.hpp"

EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop)
    : baseLoop_(baseLoop), started_(false), numThreads_(0), next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // 不需要手动释放 loop，因为它们是在栈上分配的（在 EventLoopThread::threadFunc 中）
}

// 根据设定的线程数，创建并启动相应数量的子线程，同时收集这些子线程中运行的 EventLoop 指针
void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    started_ = true;

    for (int i = 0; i < numThreads_; ++i)
    {
        auto t = std::make_unique<EventLoopThread>(loopOptions_,
                                                   cb); // 创建一个新的 EventLoopThread 对象，传入线程初始化回调
        loops_.push_back(t->startLoop());
        LOG_INFO("ThreadPool started worker {}, loop={}", i, static_cast<void *>(loops_.back()));
        threads_.push_back(std::move(t)); // 将线程对象添加到线程池中，使用 move 语义转移所有权
    }

    if (numThreads_ == 0 && cb)
    {
        cb(baseLoop_);
    }
}

EventLoop *EventLoopThreadPool::getNextLoop()
{
    EventLoop *loop = baseLoop_;

    if (!loops_.empty())
    {
        // 轮询
        loop = loops_[next_];
        ++next_;
        if (static_cast<size_t>(next_) >= loops_.size()) // 环形
        {
            next_ = 0;
        }
    }

    return loop;
}

std::vector<EventLoop *> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())
    {
        return std::vector<EventLoop *>(1, baseLoop_);
    }
    else
    {
        return loops_;
    }
}
