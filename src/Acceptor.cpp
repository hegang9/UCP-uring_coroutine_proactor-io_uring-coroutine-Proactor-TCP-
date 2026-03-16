#include <cstring>
#include <errno.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "Acceptor.hpp"
#include "EventLoop.hpp"
#include "InetAddress.hpp"
#include "Logger.hpp"

// 创建一个非阻塞的监听Socket文件描述符
static int createNonblockingSocket()
{
    int listenSocketFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listenSocketFd < 0)
    {
        // TODO: 这里可以考虑抛出异常或者返回错误码，当前简单处理为日志记录和终止程序
        LOG_ERROR("Acceptor socket create failed: {}", std::strerror(errno));
        abort();
    }
    return listenSocketFd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : acceptLoop_(loop), listenSocket_(createNonblockingSocket()), listening_(false),
      clientAddrLen_(sizeof(clientAddr_)), acceptContext_(IoType::Accept, listenSocket_.getFd())
{
    // 允许地址重用，防止TIME_WAIT导致绑定失败
    listenSocket_.setReuseAddr(true);
    // 端口重用，为以后扩展多线程acceptor做准备
    listenSocket_.setReusePort(reuseport);
    // 绑定监听地址
    listenSocket_.bindAddress(listenAddr);

    // 绑定回调函数
    acceptContext_.handler = std::bind(&Acceptor::handleRead, this, std::placeholders::_1);
}

Acceptor::~Acceptor()
{
    if (listening_)
    {
        ::close(listenSocket_.getFd());
    }
}

void Acceptor::listen()
{
    listening_ = true;
    listenSocket_.listen();
    asyncAccept(); // 提交第一个 accept 请求
}

void Acceptor::asyncAccept()
{
    // 获取 SQE
    struct io_uring_sqe *sqe = io_uring_get_sqe(&(acceptLoop_->ring_));
    if (!sqe)
    {
        // 如果 SQ 满了，可能需要处理错误或重试
        // 这里简单处理，实际项目中可能需要更健壮的错误处理
        LOG_ERROR("Acceptor asyncAccept failed: no SQE available, err={}", std::strerror(errno));
        return;
    }

    // 准备 ACCEPT 操作
    io_uring_prep_accept(sqe, listenSocket_.getFd(), (struct sockaddr *)&clientAddr_, &clientAddrLen_, 0);

    // 绑定上下文
    io_uring_sqe_set_data(sqe, &acceptContext_);

    // 提交 - 移除，由 Loop 统一处理
    // io_uring_submit(&(acceptLoop_->ring_));
}

void Acceptor::handleRead(int res)
{
    // res 是 io_uring 异步 accept 的返回值，即第一个新的 connfd
    if (res >= 0)
    {
        int connfd = res;
        InetAddress peerAddr(clientAddr_);
        if (newConnectionCallback_)
        {
            newConnectionCallback_(connfd, peerAddr); // 执行新连接处理回调函数
        }
        else
        {
            ::close(connfd); // 没有回调函数，关闭连接
        }

        // 方案B核心优化：不断调用非阻塞 accept4 榨干全连接队列，直到返回 EAGAIN
        while (listening_)
        {
            clientAddrLen_ = sizeof(clientAddr_);
            int nextConnfd = ::accept4(listenSocket_.getFd(), (struct sockaddr *)&clientAddr_, &clientAddrLen_,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (nextConnfd >= 0)
            {
                InetAddress nextPeerAddr(clientAddr_);
                if (newConnectionCallback_)
                {
                    newConnectionCallback_(nextConnfd, nextPeerAddr);
                }
                else
                {
                    ::close(nextConnfd);
                }
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break; // 全连接队列已空，退出循环等下一波 io_uring 通知
                }
                else if (errno == EINTR)
                {
                    continue; // 信号中断，继续尝试
                }
                else
                {
                    LOG_ERROR("Acceptor loop accept4 failed: {}", std::strerror(errno));
                    break;
                }
            }
        }
    }
    else
    {
        // io_uring accept 失败
        // 如果是 ECANCELED，说明可能是 EventLoop 正在退出
        if (res != -ECANCELED)
        {
            errno = -res;
            LOG_ERROR("Acceptor::handleRead failed: {}", std::strerror(errno));
        }
    }

    // 队列已经空了（或者发生非致命错误），只要还在监听且未被取消，就再次向 io_uring 提交下一次等待
    if (listening_ && res != -ECANCELED)
    {
        clientAddrLen_ = sizeof(clientAddr_);
        asyncAccept();
    }
}
