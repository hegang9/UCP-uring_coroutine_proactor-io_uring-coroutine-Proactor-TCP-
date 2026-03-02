#include "EventLoop.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "Logger.hpp"

// 获取当前线程ID的辅助函数 (Linux specific)
// #include <sys/syscall.h>
// static pid_t gettid()
// {
//     return static_cast<pid_t>(::syscall(SYS_gettid));
// }

namespace
{
// 把 EventLoop::Options 里的关键字段做“兜底修正”，避免用户传入 0 或非法值导致运行异常。
EventLoop::Options normalizeOptions(EventLoop::Options options)
{
    if (options.ringEntries == 0)
    {
        options.ringEntries = 1024;
    }
    if (options.pendingQueueCapacity == 0)
    {
        options.pendingQueueCapacity = 1024;
    }
    if (options.registeredBuffersCount == 0)
    {
        options.registeredBuffersCount = 1;
    }
    if (options.registeredBuffersSize == 0)
    {
        options.registeredBuffersSize = 4096;
    }
    // 修正背压水位标记
    if (options.pendingQueueHighWaterMark == 0 || options.pendingQueueHighWaterMark > options.pendingQueueCapacity)
    {
        options.pendingQueueHighWaterMark = options.pendingQueueCapacity * 90 / 100;
    }
    if (options.pendingQueueLowWaterMark == 0 || options.pendingQueueLowWaterMark >= options.pendingQueueHighWaterMark)
    {
        options.pendingQueueLowWaterMark = options.pendingQueueCapacity * 40 / 100;
    }
    return options;
}
} // namespace

EventLoop::EventLoop() : EventLoop(Options())
{
}

EventLoop::EventLoop(const Options &options)
    : options_(normalizeOptions(options)), running_(false), quit_(false), threadId_(::gettid()),
      wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)), wakeupContext_(IoType::Read, wakeupFd_),
      callingPendingFunctors_(false), pendingFunctors_{options_.pendingQueueCapacity}
{
    if (wakeupFd_ < 0)
    {
        LOG_ERROR("eventfd failed: {}", std::strerror(errno));
        abort();
    }

    // 初始化 io_uring，队列深度设为 4096
    // 开启 IORING_SETUP_SQPOLL 以消除 io_uring_submit 的系统调用开销
    // 这会启动一个内核线程来轮询 SQ Ring，极大提升高频小包场景的吞吐量
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    if (options_.sqpoll)
    {
        params.flags = IORING_SETUP_SQPOLL;
        params.sq_thread_idle = options_.sqpollIdleMs;
    }

    int ret = io_uring_queue_init_params(static_cast<unsigned int>(options_.ringEntries), &ring_, &params);
    if (ret < 0)
    {
        LOG_ERROR("io_uring_queue_init failed: {}", ret);
        abort();
    }

    // 设置 io_uring I/O完成的回调
    wakeupContext_.handler = std::bind(&EventLoop::handleWakeup, this);

    // 提交第一个 wakeup 读请求
    asyncReadWakeup();
}

EventLoop::~EventLoop()
{
    if (!registeredIovecs.empty())
    {
        int ret = io_uring_unregister_buffers(&ring_);
        if (ret < 0)
        {
            LOG_ERROR("io_uring_unregister_buffers failed: {}", ret);
        }
    }

    for (void *ptr : registeredBuffersPool)
    {
        std::free(ptr);
    }
    registeredBuffersPool.clear();
    registeredIovecs.clear();
    freeBufferIndices_.clear();

    ::close(wakeupFd_);
    io_uring_queue_exit(&ring_);
}

void EventLoop::loop()
{
    running_ = true;
    quit_ = false;

    while (!quit_)
    {
        // 仅在有待提交 SQE 时提交，减少无效系统调用
        // 必须在等待之前提交，否则内核不知道有新请求，可能死锁
        if (io_uring_sq_ready(&ring_) > 0)
        {
            io_uring_submit(&ring_);
        }

        struct io_uring_cqe *cqe;
        // 等待至少一个事件完成
        int ret = io_uring_wait_cqe(&ring_, &cqe);

        if (ret < 0)
        {
            if (ret == -EINTR)
                continue; // 被信号中断
            LOG_ERROR("io_uring_wait_cqe error: {}", ret);
            break;
        }

        // 处理完成队列中的所有事件
        unsigned head;      // CQE 队列头
        unsigned count = 0; // 完成的事件数量

        // 批量遍历 CQE
        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            handleCompletionEvent(cqe);
        }

        // 推进 CQ 队列
        io_uring_cq_advance(&ring_, count);

        // 执行任务队列中的任务
        doPendingFunctors();
    }

    running_ = false;
}

void EventLoop::quit()
{
    quit_ = true;
    if (::gettid() != threadId_)
    {
        wakeup();
    }
}

void EventLoop::runInLoop(Functor cb)
{
    if (::gettid() == threadId_)
    {
        cb();
    }
    else
    {
        queueInLoop(std::move(cb));
    }
}

// 将任务放入跨线程任务队列（pendingFunctors_）中，并唤醒目标 EventLoop 线程执行
// 包含队列级别的背压机制：防止主线程分发任务过快，导致工作线程队列积压 OOM
void EventLoop::queueInLoop(Functor cb)
{
    // 获取当前队列大小（无锁队列的 size() 是近似值，但用于背压判断足够了）
    size_t curQueueSize = pendingFunctors_.size();

    // 尝试入队，如果队列已满（达到 capacity），则丢弃任务并告警
    if (!pendingFunctors_.enqueue(std::move(cb)))
    {
        // 队列满了，统计告警
        if (options_.enableQueueFullStats)
        {
            backpressureStats_.queueFullCount++;
            LOG_ERROR("EventLoop: pending queue full! queueSize={}, capacity={}, droppedCount={}", curQueueSize,
                      options_.pendingQueueCapacity, backpressureStats_.queueFullCount);
        }
        return;
    }

    // 统计：记录队列达到的最大峰值，便于后续调优
    backpressureStats_.maxPendingQueueSize = std::max(backpressureStats_.maxPendingQueueSize, curQueueSize);

    // 检查是否触发高水位阈值
    bool isHighWaterMark = curQueueSize >= options_.pendingQueueHighWaterMark;
    bool wasInHighWaterMark = inHighWaterMark_.load(std::memory_order_relaxed);

    if (isHighWaterMark && !wasInHighWaterMark)
    {
        // 状态跃迁：从正常状态进入高水位状态
        inHighWaterMark_.store(true, std::memory_order_relaxed);
        backpressureStats_.highWaterMarkEvents++;
        LOG_WARN("EventLoop: entering high water mark, queueSize={}, threshold={}", curQueueSize,
                 options_.pendingQueueHighWaterMark);
        // 触发高水位回调，业务层可在此回调中暂停接收新连接或降低任务生产速率
        if (backpressureCallback_)
        {
            backpressureCallback_(true);
        }
    }
    else if (!isHighWaterMark && wasInHighWaterMark && curQueueSize <= options_.pendingQueueLowWaterMark)
    {
        // 状态跃迁：从高水位状态恢复到正常状态（必须降至低水位以下才算恢复，防止在阈值附近频繁震荡）
        inHighWaterMark_.store(false, std::memory_order_relaxed);
        backpressureStats_.lowWaterMarkEvents++;
        LOG_WARN("EventLoop: leaving high water mark, queueSize={}, threshold={}", curQueueSize,
                 options_.pendingQueueLowWaterMark);
        // 触发低水位回调，业务层可在此回调中恢复接收新连接或恢复任务生产
        if (backpressureCallback_)
        {
            backpressureCallback_(false);
        }
    }

    // 如果不在当前线程，或者当前正在执行 pendingFunctors，都需要唤醒
    if (::gettid() != threadId_ || callingPendingFunctors_)
    {
        wakeup();
    }
}

void EventLoop::handleCompletionEvent(io_uring_cqe *cqe)
{
    void *data = io_uring_cqe_get_data(cqe);
    if (!data) // 安全检查
    {
        return;
    }
    IoContext *ctx = static_cast<IoContext *>(data);

    // Cancel CQE 安全检查：如果 IoContext 绑定了 TcpConnection，检查连接是否还活着
    // 只有 TcpConnection 的读写 IO 才绑定了 connection（Acceptor/Wakeup 的 connection 为空）
    if (ctx->connection.owner_before(std::weak_ptr<TcpConnection>{}) ||
        std::weak_ptr<TcpConnection>{}.owner_before(ctx->connection))
    {
        // connection 曾被设置过（非空），检查是否还活着
        if (ctx->connection.expired())
        {
            // TcpConnection 已销毁，忽略这个 CQE，避免野指针访问
            return;
        }
    }

    int result = cqe->res;
    ctx->result_ = result;

    // 优先检查是否是协程模式
    if (ctx->coro_handle)
    {
        // 协程模式：保存结果到 Awaitable 对象，然后恢复协程，用于 Read/Write 的协程接口，业务逻辑
        ctx->coro_handle.resume(); // 恢复协程执行
    }
    else if (ctx->handler)
    {
        // 传统回调模式，用于Acceptor的连接事件和 EventLoop的唤醒事件
        ctx->handler(result);
    }
}

// 写eventfd以唤醒子线程EventLoop循环
void EventLoop::wakeup()
{
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one))
    {
        LOG_ERROR("EventLoop::wakeup write failed: {}", std::strerror(errno));
    }
}

void EventLoop::initRegisteredBuffers()
{
    registeredBuffersPool.resize(options_.registeredBuffersCount);
    registeredIovecs.resize(options_.registeredBuffersCount);
    freeBufferIndices_.reserve(options_.registeredBuffersCount);

    // 页对齐分配
    for (size_t i = 0; i < options_.registeredBuffersCount; ++i)
    {
        void *ptr = nullptr;
        if (posix_memalign(&ptr, 4096, options_.registeredBuffersSize) != 0)
        {
            LOG_ERROR("initRegisteredBuffers: posix_memalign failed");
            throw std::bad_alloc();
        }
        registeredBuffersPool[i] = ptr;
        registeredIovecs[i].iov_base = ptr;
        registeredIovecs[i].iov_len = options_.registeredBuffersSize;
        freeBufferIndices_.push_back(static_cast<int>(i));
    }
    // 注册到 io_uring
    int ret = io_uring_register_buffers(&ring_, registeredIovecs.data(),
                                        static_cast<unsigned int>(options_.registeredBuffersCount));
    if (ret < 0)
    {
        LOG_ERROR("io_uring_register_buffers failed: {}", ret);
    }
}

int EventLoop::getRegisteredBufferIndex()
{
    // 单线程无锁操作：直接操作 vector 尾部，O(1) 且无竞争
    if (freeBufferIndices_.empty())
    {
        return -1;
    }
    int idx = freeBufferIndices_.back();
    freeBufferIndices_.pop_back();
    return idx;
}

void EventLoop::returnRegisteredBuffer(int idx)
{
    // 单线程无锁操作
    freeBufferIndices_.push_back(idx);
}

void *EventLoop::getRegisteredBuffer(int idx)
{
    return registeredBuffersPool[idx];
}

void EventLoop::handleWakeup()
{
    // 重新提交 wakeup 读请求，以便下一次唤醒
    asyncReadWakeup();
}

void EventLoop::asyncReadWakeup()
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        // 极其罕见的情况：SQ 满了。
        // 实际生产中可能需要处理，这里简单打印
        LOG_ERROR("EventLoop::asyncReadWakeup: SQ full");
        return;
    }

    // 准备读取 eventfd
    io_uring_prep_read(sqe, wakeupFd_, &wakeupBuffer_, sizeof(uint64_t), 0);
    io_uring_sqe_set_data(sqe, &wakeupContext_);
    // io_uring_submit(&ring_); // 移除通过 Loop 统一提交
}

void EventLoop::doPendingFunctors()
{
    callingPendingFunctors_ = true;

    // 批量出队到本地
    std::vector<Functor> functors;
    // 预留足够空间，避免频繁分配
    functors.reserve(4096);

    Functor f;
    // 关键修复：移除大小限制，尽可能排空队列，防止积压
    // 为了防止饿死IO，可以设一个较大的上限 (如 65536)
    int limit = 65536;
    while (limit-- > 0 && pendingFunctors_.dequeue(f))
    {
        functors.emplace_back(std::move(f));
    }

    // 无锁执行（此时生产者仍可入队到剩余队列）
    for (auto &func : functors)
    {
        func();
    }

    callingPendingFunctors_ = false;
}

EventLoop::BackpressureStats EventLoop::getBackpressureStats() const
{
    return backpressureStats_;
}

void EventLoop::resetBackpressureStats()
{
    backpressureStats_ = BackpressureStats();
}
