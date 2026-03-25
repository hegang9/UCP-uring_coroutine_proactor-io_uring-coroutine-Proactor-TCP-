#include "AsyncSleep.hpp"

void AsyncSleepAwaitable::await_suspend(std::coroutine_handle<> handle)
{
    // 绑定需要被唤醒的协程句柄
    timeoutContext_.coro_handle = handle;

    // 提交超时任务给io_uring
    // 1. 获取一个提交队列条目
    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop_->ring_);
    if (!sqe)
    {
        // TODO
        // 极其罕见的情况：SQ 满了。在项目中通常打印日志并直接恢复协程（或抛出异常/结束应用）
        // 实际生产中由于批量提交机制，SQ被填满的概率极低。
        handle.resume();
        return;
    }

    // 2. 准备timeout请求
    // 第三个参数count=0表示纯粹的定时器，不依赖完成事件数量
    // 第四个参数flag=0表示默认行为，使用相对时间计算超时
    io_uring_prep_timeout(sqe, &ts_, 0, 0);

    // 3. 将IoContext与sqe关联，以便超时完成时能正确唤醒协程
    io_uring_sqe_set_data(sqe, &timeoutContext_);

    // 4. 提交请求
    /*
        由于系统开启了io_uring的SQPOLL模式，通常情况下调用io_uring_submit只会执行一道内存屏障，将 SQ
       队列的尾指针往后移动，让轮询的内核线程能够看到你提交的SQE并开始计时。
        即使关闭了SQPOLL模式，调用io_uring_submit触发系统调用进入内核，对于定时器等极度依赖精准触发时间的场景，也强烈建议在这里立即提交。这消除了将SQE驻留在提交队列中等待EventLoop后续批量提交而产生的不可控的延迟误差。但是这会导致混合负载场景下的批处理退化问题，即普通批处理读写请求和非批处理的定时器请求交织在一起，可能会增加系统调用的频率，降低吞吐量。针对这个问题，可以考虑以下优化策略：
        1.
       优先级队列：为定时器请求和普通IO请求分别维护两个提交队列，定时器请求优先提交，普通IO请求批量提交。这样可以保证定时器的精准触发，同时在负载较高时仍能保持较高的吞吐量。
        2. 牺牲一定的时间精度，信任loop批处理机制的提交速度，在此处不进行submit
        TODO: 实现优先级队列机制
    */
    io_uring_submit(&loop_->ring_);
}