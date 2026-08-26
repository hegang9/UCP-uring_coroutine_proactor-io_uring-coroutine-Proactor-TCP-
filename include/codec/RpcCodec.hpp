#pragma once
#include "Buffer.hpp"
#include <cstdint>
#include <string>
#include <vector>

// ===================== RPC 数据结构定义 =====================

/**
 * @brief 简单的 RPC 消息结构
 *
 * 协议格式 (Length Field Based):
 * +----------------+----------------+
 * | Length (4 bytes)|  Payload (Body) |
 * +----------------+----------------+
 *
 * Length: 4字节大端整数，表示 Payload 的长度（不包含 Length 本身）
 */
struct RpcMessage {
  // 消息类型，例如 0=Request, 1=Response, 2=Heartbeat
  int type = 0;

  // 序列号，用于请求响应匹配
  uint64_t id = 0;

  // 具体的业务数据 (可能是 protobuf, json, msgpack 等序列化后的字节流)
  std::string payload;

  void reset() {
    type = 0;
    id = 0;
    payload.clear();
  }
};

// ===================== Codec 接口定义 =====================

/**
 * @brief RPC 编解码器
 * 负责处理基于长度前缀的二进制协议分包与编解码
 */
class RpcCodec {
public:
  // 解析结果枚举
  enum class DecodeResult {
    kComplete,   // 解析出一个完整包
    kIncomplete, // 数据不足（拆包），需要更多数据
    kError       // 解析错误（例如长度超过限制）
  };

  /**
   * @brief 尝试从 buffer 中解析出一个 RPC 消息
   *
   * @param buf 输入缓冲区
   * @param outMsg 输出参数，解析成功时填充
   * @return DecodeResult
   */
  static DecodeResult decode(Buffer *buf, RpcMessage &outMsg);

  /**
   * @brief 将 RPC 消息编码并追加到 buffer 中
   *
   * @param buf 输出缓冲区
   * @param msg 待发送的消息对象
   */
  static void encode(Buffer *buf, const RpcMessage &msg);

  // 最大允许的消息体长度，防止恶意攻击或异常包导致 OOM
  static const size_t kMaxMessageLength = 64 * 1024 * 1024; // 64MB

  // 头部长度 (4字节 TotalLength + 4字节 Type + 8字节 ID)
  static const size_t kHeaderLength = 4 + 4 + 8;
};
