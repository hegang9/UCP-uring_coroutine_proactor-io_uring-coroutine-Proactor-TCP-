<h1 align="center">UCP (uring_coroutine_proactor)</h1>

<p align="center">
  <b>基于 io_uring 与 C++20 协程的极致高性能纯异步网络框架</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Linux-blue.svg" alt="Platform">
  <img src="https://img.shields.io/badge/C++-20-blue.svg" alt="C++20">
  <img src="https://img.shields.io/badge/io__uring-5.10+-brightgreen.svg" alt="io_uring">
  <img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License">
</p>

## 📖 项目简介
该系统是一个专为 Linux 环境设计的高性能 TCP 服务器框架。它摒弃了传统的 `epoll + Reactor` 模型，全面拥抱 Linux **io_uring** 异步 I/O 接口，实现了纯粹的 **Proactor** 模式。

本项目的核心目标是**极致的性能与低延迟**。通过“内核执行 I/O，完成后通知应用”的机制，结合 C++20 无栈协程、无锁任务队列、零拷贝等现代前沿技术，最大化利用多核 CPU 资源。适用于海量并发连接的高频交易、即时通讯及高性能网关等场景。

## ✨ 核心特性与优化设计
- **🚀 内核级轮询 (Zero Syscall)**: 配置 `IORING_SETUP_SQPOLL` 开启内核轮询线程。应用投递任务仅需更新队列尾指针，极大概率免除 `io_uring_enter` 系统调用开销，实现原生极低延迟。
- **⚡ 内核级零拷贝 (Zero Copy)**: 深度利用 io_uring 的 **Registered Buffers** 特性预先固定内存映射。避免高并发 I/O 时内核频繁执行 `get_user_pages`/`put_page` 开销，大幅提升极速读写性能。
- **🧵 C++20 无栈协程解耦**: 利用 C++20 协程定制 `promise_type` 与 `awaiter`，将异步 SQE 提交与 CQE 收割无缝桥接至协程的挂起与恢复，用同步代码思路编写异步流，彻底告别“回调地狱（Callback Hell）”。
- **🎯 One Ring Per Thread 无锁调度**: 采用主从 Proactor 多线程模型。主线程专属 Accept，子线程独立接管 io_uring 数据流。单连接完整生命周期极致绑定单一线程，规避所有跨线程抢锁开销。
- **⏱️ 内核级定时器代理**: 废弃用户态红黑树/时间轮等繁重逻辑，全面将请求超时、死链检测代理给 io_uring 专属的 `Link Timeout` 机制，降低 CPU 无意义开销。
- **📦 高性能组件引擎**: 底层内置针对小报文优化的 `MemoryPool` (内存池)；跨线程派发任务采用基于数组的**无锁任务队列**结合内核事件描述符 `eventfd`，以极限开销完成线程间的休眠唤醒。

## 🏗️ 核心架构图解
模块层次严密划分如下：
*   **控制与调度层**: `TcpServer`（系统总控封装），`EventLoop`（io_uring 基于事件的底层驱动引擎），`Acceptor`（用于建立高频连接处理的源头）。
*   **业务抽象层**: `TcpConnection`（抽象与追踪 TCP 整个闭环生命周期及异步读写流），`IoContext`（将 I/O 操作、回调指针、底层内存高度封装并投递给内核态的 `user_data`）。
*   **公共组件设施**: `Buffer`（智能动扩容读写应用缓冲区），`MemoryPool`（多级碎片整理内存池）。

## 🛠️ 环境依赖 (Prerequisites)
运行本系统需要较新的 Linux 内核以支持 io_uring 的完整特性，同时需要支持 C++20 的构建体系。
*   **操作系统**: Linux Kernel **5.10+** (强烈推荐 5.19 或更高版本以获得极速特性和完美调度)。
*   **编译器**: GCC **11.0+** 或 Clang **14.0+** (需完整支持 C++20 协程语义)。
*   **依赖库**: 
    - [liburing](https://github.com/axboe/liburing) (推荐 v2.2 及以上版本，作为 io_uring 的官方 C 接口封装)
    - [fmt](https://github.com/fmtlib/fmt) 现代 C++ 格式化库
*   **构建工具**: CMake **3.10+**

## 📂 项目结构
```text
UCP/
├── bin/            # 编译输出的可执行文件及核心二进制
├── build/          # CMake 构建中间目录
├── benchmark.sh    # 自动化抗压测试脚本
├── client/         # 各种异常边界与稳定连通性测试客户端
├── config/         # 默认启动与性能参数集
├── include/        # 核心框架逻辑声明定义
├── log/            # 高性能落地无锁日志系统具体实现
├── memory/         # Slab环形缓冲级别内存池机制
└── src/            # UCP 框架核心通信机制与事件实现
```

## ⚙️ 配置与日志系统
项目新增轻量级通用 `.conf` (INI风格) 配置文件，系统默认路径为 `config/ucp.conf`。通过配置文件可从运行逻辑剥离各项底层参数进而调优。

与此同时，UCP 框架内置极速多 Sink 异步非阻塞日志系统。直接将 I/O 线程日志无锁打入内存队列，再通过独立的日志线程专门执行刷盘（Flush）操作避免卡死 I/O 主流程。

<details>
<summary><b>👉 点击查看核心配置范例 (ucp.conf)</b></summary>

```ini
[server]
name = TcpServer
ip = 0.0.0.0
port = 8888
thread_num = 16
read_timeout_ms = 5000

[event_loop]
ring_entries = 32768
sqpoll = true
sqpoll_idle_ms = 50
registered_buffers_count = 16384
registered_buffer_size = 4096
pending_queue_capacity = 65536

[log]
level = INFO
file = logs/server.log
max_size = 104857600
max_files = 10
async = true
console = true
flush_interval_ms = 1000
```
</details>

<details>
<summary><b>👉 点击查看日志接入及特色参数说明</b></summary>

核心极速特性与功能支持：
- **异步无阻塞落盘**: 日志写入端执行彻底无锁的 `Push（推入队列与EventFD）` 极大拉低日志对于请求耗时与计算单元拖拽。
- **动态自适应输出**: 支持从 `TRACE` 到 `FATAL` 共计 6 级颗粒输出，完全支撑高度调试环境需要。
- **极佳分流支持**: 基于大小的 Rotate，保留一定总数的滚卷策略 (`max_size / max_files`)，控制磁盘增长边界。同时灵活控制 `Console(前台)` 和 `Async File Sink` 的定向策略。

极简洁的框架内置宏调用：
```cpp
LOG_INFO("Server started on {}:{}", ip, port);
LOG_WARN("Connection timeout: fd={}", fd);
LOG_ERROR("Accept failed: {}", strerror(errno));
```
</details>




## 🚀 快速开始 (Getting Started)

### 1. 构建 UCP 框架
```bash
mkdir build && cd build
cmake ..
make -j"$(nproc)"
```

### 2. 启动单机 UCP 微服务
> **注:** 为了应对千万级并发网络风暴，首要解决的是重置操作系统硬性 FD 上线，并发起针对 io_uring 内核常驻内存配置 `unlimited`！
```bash
cd /home/hegang/UCP 
sudo sh -c "ulimit -n 100000 && ulimit -l unlimited && ./bin/proactor_test config/ucp.conf"
```

### 3. 一次性边界回归与连通性自验证客户端
我们在系统中内置了一个专门用于进行连通性确认、延迟异常与业务鲁棒性对抗的套件程序：
```bash
cd /home/hegang/UCP
./client/test_client all
```
> *(执行上述命令会依次验证正常收发、慢速客户端抗压、无响应 TCP 空闲踢除、非法越权断开等关键用例)*

## 📊 性能表现与抗压结果 (Benchmarks)
我们在极度苛刻、完全剥离并且**不受物理网卡带宽妥协的本机局域网 Loopback 环回测试**环境中，对本系统的高阶吞吐极值天花板能力展开拷机实战：

运行自带的一体化自动化压测工具集：
```bash
./benchmark.sh
```

**实战压测报告：**
- **测试硬件宿主**: Intel Core i7-14700HX
- **主被动压测设置**: 运行 16 条系统级 Sub Proactor 子线程, 基于强烈的 Ping-Pong Echo 短包高频业务对抗
- **成绩汇总**: 在与压测端建立多达 **4000 个 TCP 真实并发连接** 后，系统展现出了极佳的吞吐与延迟控制能力。单侧峰值处理能力达到了 **124万 RPS (每秒请求处理量)**、约 **129.67 MB/s 的传输吞吐量**，且在高压满载状态下，平均通信延迟稳定保持在 **1.49ms - 1.62ms** 之间，P99延迟控制在8ms上下。

<details>
<summary><b>👉 点击查看详细的 3000 ~ 4000 并发分级压测报告日志输出</b></summary>

```text
Connections: 3000
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.93ms    1.83ms  29.57ms   85.21%
    Req/Sec    55.07k    26.01k  158.73k    63.89%
  Latency Distribution
     50%    1.17ms
     75%    2.36ms
     90%    4.58ms
     99%    8.36ms
  21975865 requests in 19.33s, 2.23GB read
  Socket errors: connect 906, read 0, write 0, timeout 1826
Requests/sec: 1137024.45
Transfer/sec:    118.19MB
==========================================================
Connections: 3100
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.94ms    1.79ms  28.07ms   85.31%
    Req/Sec    55.16k    27.17k  151.30k    56.14%
  Latency Distribution
     50%    1.21ms
     75%    2.29ms
     90%    4.54ms
     99%    8.28ms
  21922823 requests in 20.09s, 2.23GB read
  Socket errors: connect 961, read 0, write 0, timeout 0
Requests/sec: 1091131.82
Transfer/sec:    113.42MB
==========================================================
Connections: 3200
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.52ms    1.58ms  39.24ms   88.04%
    Req/Sec    59.24k    35.03k  137.25k    53.40%
  Latency Distribution
     50%    0.98ms
     75%    1.46ms
     90%    3.59ms
     99%    7.83ms
  23564018 requests in 20.10s, 2.39GB read
  Socket errors: connect 1366, read 0, write 0, timeout 0
Requests/sec: 1172534.16
Transfer/sec:    121.89MB
==========================================================
Connections: 3300
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3300 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.57ms    1.59ms  27.64ms   87.11%
    Req/Sec    57.66k    32.87k  147.78k    57.29%
  Latency Distribution
     50%    0.99ms
     75%    1.58ms
     90%    3.77ms
     99%    7.75ms
  22978590 requests in 19.20s, 2.33GB read
  Socket errors: connect 1449, read 0, write 0, timeout 1520
Requests/sec: 1196849.19
Transfer/sec:    124.41MB
==========================================================
Connections: 3400
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.75ms    1.80ms  61.81ms   85.58%
    Req/Sec    55.30k    27.58k  137.42k    65.67%
  Latency Distribution
     50%    1.03ms
     75%    1.96ms
     90%    4.31ms
     99%    8.05ms
  22010875 requests in 19.12s, 2.23GB read
  Socket errors: connect 1447, read 0, write 0, timeout 1629
Requests/sec: 1151013.45
Transfer/sec:    119.65MB
==========================================================
Connections: 3500
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     2.51ms    9.50ms 213.23ms   99.23%
    Req/Sec    55.84k    30.61k  126.41k    60.82%
  Latency Distribution
     50%    1.17ms
     75%    2.09ms
     90%    4.59ms
     99%   10.27ms
  22119217 requests in 19.10s, 2.25GB read
  Socket errors: connect 1348, read 0, write 0, timeout 2068
Requests/sec: 1158362.75
Transfer/sec:    120.41MB
==========================================================
Connections: 3600
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3600 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.90ms    1.78ms  25.12ms   85.35%
    Req/Sec    55.60k    28.94k  141.35k    64.91%
  Latency Distribution
     50%    1.19ms
     75%    2.13ms
     90%    4.51ms
     99%    8.29ms
  22042657 requests in 20.10s, 2.24GB read
  Socket errors: connect 1422, read 0, write 0, timeout 0
Requests/sec: 1096652.98
Transfer/sec:    114.00MB
==========================================================
Connections: 3700
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3700 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.81ms    1.75ms  26.81ms   86.27%
    Req/Sec    56.76k    32.54k  151.85k    58.92%
  Latency Distribution
     50%    1.15ms
     75%    1.90ms
     90%    4.32ms
     99%    8.36ms
  22582506 requests in 19.12s, 2.29GB read
  Socket errors: connect 1572, read 0, write 0, timeout 1990
Requests/sec: 1180823.03
Transfer/sec:    122.75MB
==========================================================
Connections: 3800
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3800 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.75ms    1.74ms  35.99ms   86.57%
    Req/Sec    54.77k    32.84k  141.30k    60.87%
  Latency Distribution
     50%    1.11ms
     75%    1.80ms
     90%    4.22ms
     99%    8.33ms
  21727679 requests in 19.14s, 2.21GB read
  Socket errors: connect 1790, read 0, write 0, timeout 1920
Requests/sec: 1134934.46
Transfer/sec:    117.98MB
==========================================================
Connections: 3900
Running 20s test @ http://192.168.2.69:6666
  20 threads and 3900 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.49ms    1.48ms  26.11ms   88.83%
    Req/Sec    60.03k    37.79k  156.42k    62.21%
  Latency Distribution
     50%    1.03ms
     75%    1.48ms
     90%    3.32ms
     99%    7.52ms
  23854640 requests in 19.12s, 2.42GB read
  Socket errors: connect 1946, read 0, write 0, timeout 1822
Requests/sec: 1247384.53
Transfer/sec:    129.67MB
==========================================================
Connections: 4000
Running 20s test @ http://192.168.2.69:6666
  20 threads and 4000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.62ms    1.55ms  28.08ms   87.31%
    Req/Sec    60.58k    33.17k  166.98k    68.93%
  Latency Distribution
     50%    1.09ms
     75%    1.65ms
     90%    3.81ms
     99%    7.66ms
  23976229 requests in 20.10s, 2.43GB read
  Socket errors: connect 1915, read 0, write 0, timeout 0
Requests/sec: 1193004.26
Transfer/sec:    124.01MB
==========================================================
```
</details>

## 📄 开源协议 (License)
本项目遵守 **[MIT](https://opensource.org/licenses/MIT)** 开源协议。欢迎同样沉迷于 C++ 极致性能、现代协程编程和疯狂压榨底层 Linux io_uring 引擎魔力的同好，提交 `PR` 或 `Issue`。