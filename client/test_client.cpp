#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

// 辅助函数：创建一个连接到指定IP和端口的TCP Socket
int create_connection(const string &ip, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr) <= 0)
    {
        perror("Invalid address/ Address not supported");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("Connection Failed");
        close(sock);
        return -1;
    }
    return sock;
}

// 1. 功能自测：正常发送和接收数据 (HTTP GET测试)
void test_normal_echo(const string &ip, int port)
{
    cout << "[测试 1] 正常HTTP GET测试开始..." << endl;
    int sock = create_connection(ip, port);
    if (sock < 0)
        return;

    string msg = "GET / HTTP/1.1\r\nHost: " + ip + ":" + to_string(port) + "\r\nConnection: close\r\n\r\n";
    send(sock, msg.c_str(), msg.length(), 0);
    cout << "  -> 发送 HTTP 请求:\n" << msg;

    char buffer[4096] = {0};
    int valread = read(sock, buffer, 4096);
    if (valread > 0)
    {
        cout << "  <- 接收 HTTP 响应 (前100字节): \n" << string(buffer, min(valread, 100)) << "..." << endl;
    }
    else
    {
        cout << "  <- 接收失败或连接关闭" << endl;
    }

    close(sock);
    cout << "[测试 1] 正常HTTP GET测试结束。\n" << endl;
}

// 2. 异常测试：慢速客户端 (模拟网络极差，每次只发1个字节)
void test_slow_client(const string &ip, int port)
{
    cout << "[测试 2] 慢速客户端测试开始 (每次发送1字节，间隔100ms)..." << endl;
    int sock = create_connection(ip, port);
    if (sock < 0)
        return;

    string msg = "GET / HTTP/1.1\r\nHost: " + ip + ":" + to_string(port) + "\r\nConnection: close\r\n\r\n";
    for (char c : msg)
    {
        send(sock, &c, 1, 0);
        cout << "  -> 发送字节: " << (c == '\r' ? "\\r" : (c == '\n' ? "\\n" : string(1, c))) << endl;
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    char buffer[4096] = {0};
    // 设置一个较短的超时时间来读取，防止一直阻塞
    struct timeval tv;
    tv.tv_sec = 2; // 2秒超时
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

    int valread = read(sock, buffer, 4096);
    if (valread > 0)
    {
        cout << "  <- 接收 HTTP 响应 (前100字节): \n" << string(buffer, min(valread, 100)) << "..." << endl;
    }
    else
    {
        cout << "  <- 接收超时或连接关闭 (可能是服务器超时断开了连接)" << endl;
    }

    close(sock);
    cout << "[测试 2] 慢速客户端测试结束。\n" << endl;
}

// 3. 异常测试：半开连接/空闲连接 (连接后什么都不做，测试服务器的超时踢出机制)
void test_idle_connection(const string &ip, int port)
{
    cout << "[测试 3] 空闲连接测试开始 (连接后等待，测试服务器超时断开)..." << endl;
    int sock = create_connection(ip, port);
    if (sock < 0)
        return;

    cout << "  -> 已连接，使用 read() 等待服务器主动断开 (超时配置=5s)..." << endl;

    // 正确方式：用 read() 检测服务端是否发送了 FIN
    // 设置一个比服务端超时稍长的读超时（比如 10 秒），防止无限阻塞
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

    auto start = chrono::steady_clock::now();
    char buffer[1024];
    int valread = read(sock, buffer, sizeof(buffer));
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start);

    if (valread == 0)
    {
        // read() 返回 0 表示对端发送了 FIN（正常关闭）
        cout << "  -> read() 返回 0，服务器在 " << elapsed.count() << "ms 后主动关闭了连接，符合预期 ✅" << endl;
    }
    else if (valread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        // 读超时，说明服务器 10 秒内没有关闭连接
        cout << "  -> read() 超时 (" << elapsed.count() << "ms)，服务器未主动关闭连接，不符合预期 ❌" << endl;
    }
    else if (valread < 0)
    {
        // 其他错误（如 ECONNRESET），也说明服务器关闭了连接
        cout << "  -> read() 返回错误: " << strerror(errno) << "，" << elapsed.count() << "ms 后连接被关闭 ✅" << endl;
    }
    else
    {
        // 收到了数据，不应该发生（空闲连接不应有数据）
        cout << "  -> 意外收到 " << valread << " 字节数据" << endl;
    }

    close(sock);
    cout << "[测试 3] 空闲连接测试结束。\n" << endl;
}

// 4. 异常测试：客户端异常断开 (发送一半数据直接close，或者直接RST)
void test_abrupt_close(const string &ip, int port)
{
    cout << "[测试 4] 异常断开测试开始 (发送部分HTTP请求后直接关闭Socket)..." << endl;
    int sock = create_connection(ip, port);
    if (sock < 0)
        return;

    string msg = "GET / HTTP/1.1\r\nHost: " + ip;
    send(sock, msg.c_str(), msg.length(), 0);
    cout << "  -> 发送部分HTTP请求: " << msg << endl;

    // 开启 SO_LINGER 并设置 l_linger=0，这样 close 时会发送 RST 包而不是 FIN 包
    struct linger sl;
    sl.l_onoff = 1;
    sl.l_linger = 0;
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));

    cout << "  -> 立即发送 RST 强制关闭连接" << endl;
    close(sock);

    cout << "[测试 4] 异常断开测试结束。\n" << endl;
}

// 5. 异常测试：并发连接风暴 (瞬间发起大量连接)
void test_connection_storm(const string &ip, int port, int num_connections)
{
    cout << "[测试 5] 并发连接风暴测试开始 (瞬间发起 " << num_connections << " 个连接)..." << endl;
    vector<int> sockets;

    for (int i = 0; i < num_connections; ++i)
    {
        int sock = create_connection(ip, port);
        if (sock >= 0)
        {
            sockets.push_back(sock);
        }
    }
    cout << "  -> 成功建立 " << sockets.size() << " 个连接" << endl;

    // 保持一小段时间后全部关闭
    this_thread::sleep_for(chrono::milliseconds(500));

    for (int sock : sockets)
    {
        close(sock);
    }
    cout << "  -> 所有连接已关闭" << endl;
    cout << "[测试 5] 并发连接风暴测试结束。\n" << endl;
}

void print_usage(const char *prog_name)
{
    cout << "用法: " << prog_name << " [测试模式] [IP] [端口]\n"
         << "测试模式:\n"
         << "  1 : 正常Echo测试\n"
         << "  2 : 慢速客户端测试\n"
         << "  3 : 空闲连接测试 (测试超时踢出)\n"
         << "  4 : 异常断开测试 (RST)\n"
         << "  5 : 并发连接风暴测试\n"
         << "  all : 运行所有测试 (默认)\n"
         << "示例: " << prog_name << " all 127.0.0.1 6666\n";
}

int main(int argc, char const *argv[])
{
    string mode = "all";
    string ip = "192.168.2.69";
    int port = 6666; // 默认读取自 ucp.conf

    if (argc > 1)
        mode = argv[1];
    if (argc > 2)
        ip = argv[2];
    if (argc > 3)
        port = stoi(argv[3]);

    if (mode == "help" || mode == "-h")
    {
        print_usage(argv[0]);
        return 0;
    }

    cout << "=== UCP 客户端测试工具 ===" << endl;
    cout << "目标服务器: " << ip << ":" << port << "\n" << endl;

    if (mode == "1" || mode == "all")
        test_normal_echo(ip, port);
    if (mode == "2" || mode == "all")
        test_slow_client(ip, port);
    if (mode == "3" || mode == "all")
        test_idle_connection(ip, port);
    if (mode == "4" || mode == "all")
        test_abrupt_close(ip, port);
    if (mode == "5" || mode == "all")
        test_connection_storm(ip, port, 100); // 默认100个并发连接

    cout << "所有指定测试已完成。" << endl;
    return 0;
}
