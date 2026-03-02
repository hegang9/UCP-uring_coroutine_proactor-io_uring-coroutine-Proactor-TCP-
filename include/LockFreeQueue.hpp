#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

// MPMC环形无锁队列（多生产者多消费者）

template <typename T> class LockFreeQueue
{
  private:
    struct Slot
    {
        std::atomic<size_t> sequence; // 序列号，用于解决ABA问题
        T data;
    };

    // 使用 alignas 避免伪共享
    alignas(64) std::atomic<size_t> enqueuePos_; // 入队位置
    alignas(64) std::atomic<size_t> dequeuePos_; // 出队位置

    std::vector<Slot> buffer_; // 环形缓冲区
    size_t bufferMask_;        // 用于快速取模（要求容量是2的幂）

    static size_t roundUpToPowerOf2(size_t n); // 向上取整到2的幂
    static constexpr size_t kCacheLineSize = 64;

    template <typename U> bool enqueueImpl(U &&data);

  public:
    explicit LockFreeQueue(size_t capacity);
    ~LockFreeQueue() = default;

    // 禁止拷贝和赋值
    LockFreeQueue(const LockFreeQueue &) = delete;
    LockFreeQueue &operator=(const LockFreeQueue &) = delete;
    // 禁止移动
    LockFreeQueue(LockFreeQueue &&) = delete;
    LockFreeQueue &operator=(LockFreeQueue &&) = delete;

    bool enqueue(const T &data);
    bool enqueue(T &&data);
    bool dequeue(T &data);

    bool empty() const;
    size_t size() const; // 并发环境下只是近似大小
};

template <typename T> inline size_t LockFreeQueue<T>::roundUpToPowerOf2(size_t n)
{
    assert(n > 0);
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

template <typename T>
inline LockFreeQueue<T>::LockFreeQueue(size_t capacity)
    : buffer_(roundUpToPowerOf2(capacity)), bufferMask_(buffer_.size() - 1), enqueuePos_(0), dequeuePos_(0)
{
    // 初始化每个槽的序列号
    for (size_t i = 0; i < buffer_.size(); ++i)
    {
        buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
}

template <typename T> template <typename U> inline bool LockFreeQueue<T>::enqueueImpl(U &&data)
{
    Slot *slot;
    size_t pos = enqueuePos_.load(std::memory_order_relaxed);

    while (true)
    {
        slot = &buffer_[pos & bufferMask_]; // 利用快速取模计算槽索引
        size_t seq = slot->sequence.load(std::memory_order_acquire);
        // seq 与 pos 的差值用于判断槽位状态：0=可写，<0=队列满，>0=被其他线程占用
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos); // 计算序列号差值

        if (dif == 0)
        {
            // 槽位可用，CAS 抢占该槽位
            if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
            {
                break;
            }
        }
        else if (dif < 0)
        {
            return false; // 队列满
        }
        else
        {
            // 竞争失败，重新读取最新入队位置再重试
            pos = enqueuePos_.load(std::memory_order_relaxed);
        }
    }

    // 将数据写入槽位，再发布序列号表示“可读”
    slot->data = std::forward<U>(data);
    slot->sequence.store(pos + 1, std::memory_order_release);
    return true;
}

template <typename T> inline bool LockFreeQueue<T>::enqueue(const T &data)
{
    return enqueueImpl(data);
}

template <typename T> inline bool LockFreeQueue<T>::enqueue(T &&data)
{
    return enqueueImpl(std::forward<T>(data));
}

template <typename T> inline bool LockFreeQueue<T>::dequeue(T &data)
{
    Slot *slot;
    size_t pos = dequeuePos_.load(std::memory_order_relaxed);

    while (true)
    {
        slot = &buffer_[pos & bufferMask_]; // 计算槽索引
        size_t seq = slot->sequence.load(std::memory_order_acquire);
        // seq 与 pos+1 的差值用于判断槽位状态：0=可读，<0=队列空，>0=被其他线程占用
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1); // 计算序列号差值

        if (dif == 0)
        {
            // 槽位有数据，CAS 抢占该槽位
            if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
            {
                break;
            }
        }
        else if (dif < 0)
        {
            return false; // 队列空
        }
        else
        {
            // 竞争失败，重新读取最新出队位置再重试
            pos = dequeuePos_.load(std::memory_order_relaxed);
        }
    }

    // 取出数据后，发布新的序列号表示“可写”进入下一轮
    data = std::move(slot->data);
    slot->sequence.store(pos + buffer_.size(), std::memory_order_release);
    return true;
}

template <typename T> inline bool LockFreeQueue<T>::empty() const
{
    size_t head = dequeuePos_.load(std::memory_order_relaxed);
    size_t tail = enqueuePos_.load(std::memory_order_relaxed);
    return head == tail;
}

template <typename T> inline size_t LockFreeQueue<T>::size() const
{
    size_t head = dequeuePos_.load(std::memory_order_relaxed);
    size_t tail = enqueuePos_.load(std::memory_order_relaxed);
    return tail - head; // 并发环境下是近似值
}
