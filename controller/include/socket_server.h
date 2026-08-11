/*
 * socket_server.h —— TCP 监听 + 文本命令解析（学习精简版）
 *
 * 对应原项目 controller/source/server_thread.cpp + socket_server。
 * 原项目收的是二进制 struct(SortingStageBox)，且塞进环形缓冲交给控制线程。
 * 我们精简成：收一行可读文本命令，直接调用 EcatMaster 发起运动。
 *
 * 支持的命令（一行一条，以换行结尾）：
 *   MOVE <角度>   例如 "MOVE 90"  → 相对转动 90 度
 *   STATUS        → 回读当前实际位置 + 状态字
 */
#pragma once

#include <atomic>
#include <string>

#include "ecat_master.h"

namespace demo {

class SocketServer {
 public:
  explicit SocketServer(EcatMaster* master) : master_(master) {}

  // 阻塞式运行：监听端口、接受连接、逐行读命令并处理，直到 stop_flag 为 true。
  void RunForever(std::atomic<bool>* stop_flag);

 private:
  // 解析并执行一行命令，返回要回给客户端的应答字符串。
  std::string HandleLine(const std::string& line);

  EcatMaster* master_;
};

}  // namespace demo
