/*
 * socket_server.cpp —— TCP 监听 + 文本命令解析 实现（学习精简版）
 *
 * 用最基础的 POSIX socket API（socket/bind/listen/accept/recv/send）实现，
 * 不引入任何第三方网络库，方便你看清“一条命令是怎么从网络进到程序里”的。
 *
 * 【本文件在项目里的位置】
 *   它是“下位机”里的通信入口：跑在一个独立线程上，监听 TCP 端口，
 *   把上位机（send_command.py）发来的一行行文本命令解析出来，翻译成
 *   对 EcatMaster 的调用。真正驱动电机的活儿不在这里，而是 EcatMaster
 *   的 1ms 周期循环在另一条线程上做——本文件只负责“收命令、投请求、回应答”。
 *   数据流向：上位机 → (TCP) → 本文件 HandleLine → EcatMaster“信箱” → 周期循环 → 电机。
 *
 * 【名词解释（贯穿全文的关键概念）】
 *   - POSIX socket：Linux/Unix 提供的一套网络编程系统调用，socket() 建套接字、
 *     bind() 绑端口、listen() 挂起监听、accept() 接一个连接、recv()/send() 收发数据。
 *   - 网络字节序：网络上统一用“大端”传多字节整数，本机 CPU 未必是大端，所以端口号、
 *     IP 这类数要用 htons()/htonl()（host→network，s=16位 short，l=32位 long）转一下。
 *   - 状态字(statusword)：CiA402 伺服协议里的一个 16 位寄存器，每一位代表驱动器一种状态
 *     （是否使能、是否到位、是否报错等）。STATUS 命令回读的就是它。
 *   - std::atomic：原子变量，多个线程同时读写它不会读到“写了一半”的坏值，无需加锁。
 *     本文件靠它安全地和周期循环线程通信（见 stop_flag 与 EcatMaster 的“信箱”）。
 */
#include "socket_server.h"

#include <arpa/inet.h>    // htons/htonl 等字节序转换函数
#include <netinet/in.h>   // sockaddr_in、INADDR_ANY 等 IPv4 地址结构
#include <sys/socket.h>   // socket/bind/listen/accept/recv/send 等核心 API
#include <unistd.h>       // close()：关闭文件描述符（socket 也是一种 fd）

#include <cstring>        // std::memset：给地址结构清零
#include <sstream>        // istringstream/ostringstream：按空白切词、拼接应答字符串

#include "config.h"       // kListenPort、DegreeToCounts 等本项目参数与换算（见 config.h）
#include "log.h"          // LOG(INFO)/LOG(ERROR) 日志宏

namespace demo {

// ── HandleLine：解析并执行一行文本命令，返回要回给客户端的应答字符串 ──
// 输入：不含换行符的一整行命令（如 "MOVE 90"）。
// 输出：一行以 '\n' 结尾的应答文本（OK.../STATUS.../ERR...）。
// 被 RunForever 的读循环调用；本函数不碰网络，只做“文本 → 动作 → 文本”的翻译。
std::string SocketServer::HandleLine(const std::string& line) {
  // 用字符串流把这一行按空白切成一个个词（istringstream = 从字符串里“读”）
  std::istringstream iss(line);
  std::string cmd;
  iss >> cmd;  // 取第一个词作为命令名

  // 分支一：MOVE 命令 —— 相对转动指定角度
  if (cmd == "MOVE") {
    double angle = 0.0;
    // 条件：紧跟命令名后面能不能再读出一个数（角度）
    if (!(iss >> angle)) {
      // 兜底：MOVE 后面没带角度，属于用法错误，回一句提示、不执行动作
      return "ERR 用法: MOVE <角度>\n";
    }
    // 关键换算：角度 → 编码器计数（见 config.h 的 DegreeToCounts）
    int32_t counts = DegreeToCounts(angle);
    // 把运动请求投进 EcatMaster 的“信箱”（原子变量），由周期循环那条线程真正执行
    master_->RequestRelativeMove(counts);
    LOG(INFO) << "收到命令 MOVE " << angle << " 度 (" << counts << " counts)";
    // ostringstream = 往字符串里“写”，用来拼出这次的成功应答
    std::ostringstream oss;
    oss << "OK MOVE " << angle << " deg (" << counts << " counts)\n";
    return oss.str();
  }

  // 分支二：STATUS 命令 —— 回读当前实际位置和状态字，不产生运动
  if (cmd == "STATUS") {
    std::ostringstream oss;
    // 这两个值由周期循环线程实时更新、存在原子变量里，这里直接读快照
    oss << "STATUS pos=" << master_->last_actual_position()
        // std::hex：让后面的数按十六进制输出（状态字习惯用 0x 形式看每一位）
        << " statusword=0x" << std::hex << master_->last_status_word() << "\n";
    return oss.str();
  }

  // 兜底分支：既不是 MOVE 也不是 STATUS，属于无法识别的命令，回错误提示
  return "ERR 未知命令，支持: MOVE <角度> | STATUS\n";
}

// ── RunForever：通信线程的主体，阻塞式地建服务、收连接、逐行处理命令 ──
// 由 main.cpp 在一条独立线程上调用，会一直运行到 *stop_flag 被置为 true。
// 参数 stop_flag：外部（主线程）用来通知“该收摊了”的原子开关，指针指向共享变量。
// 整体分四步：① 建 socket → ② 绑端口 → ③ 开监听 → ④ 主循环收连接、读命令。
void SocketServer::RunForever(std::atomic<bool>* stop_flag) {
  // 1) 创建 TCP socket
  //    AF_INET = 用 IPv4；SOCK_STREAM = 面向连接的 TCP（对应 UDP 是 SOCK_DGRAM）
  //    返回值是“文件描述符”(fd)——一个代表这个套接字的整数句柄
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  // 条件：返回负数说明创建失败（比如系统资源不够），没法继续，直接退出线程
  if (listen_fd < 0) {
    LOG(ERROR) << "socket() 失败";
    return;
  }
  // 允许端口快速重用，避免程序重启时报“Address already in use”
  // setsockopt = 设置套接字选项；SO_REUSEADDR 就是“地址可重用”这个开关
  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // 2) 绑定到本机所有网卡的 kListenPort 端口
  struct sockaddr_in addr;              // IPv4 地址结构：装“监听哪个 IP、哪个端口”
  std::memset(&addr, 0, sizeof(addr));  // 先整块清零，避免残留脏数据
  addr.sin_family = AF_INET;                    // 声明这是 IPv4 地址
  addr.sin_addr.s_addr = htonl(INADDR_ANY);     // INADDR_ANY = 监听本机所有网卡；htonl 转成网络字节序
  addr.sin_port = htons(kListenPort);           // 端口号也要转成网络字节序（kListenPort 见 config.h）
  // bind：把上面这个地址绑到 socket 上；(struct sockaddr*) 是 API 要求的通用地址类型转换
  // 条件：返回负数说明绑定失败（最常见是端口已被别的进程占用）
  if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG(ERROR) << "bind() 失败，端口 " << kListenPort;
    close(listen_fd);  // 失败也要把已建的 fd 关掉，否则泄漏
    return;
  }

  // 3) 开始监听
  //    第二个参数 1 = 等待队列长度（同时最多挂 1 个还没 accept 的连接，本项目单客户端够用）
  // 条件：listen 失败同样无法提供服务，清理后退出
  if (listen(listen_fd, 1) < 0) {
    LOG(ERROR) << "listen() 失败";
    close(listen_fd);
    return;
  }
  LOG(INFO) << "socket 服务器在端口 " << kListenPort << " 监听中…";

  // 4) 主循环：接受客户端连接，逐行读命令
  // 条件：只要外部没喊停（stop_flag 仍为 false）就一直服务；load() 是原子读取当前值
  while (!stop_flag->load()) {
    // accept 阻塞等待，直到有上位机连进来；成功则返回一个专属这条连接的新 fd
    // 后两个参数传 nullptr 表示不关心对方的地址信息
    int client_fd = accept(listen_fd, nullptr, nullptr);
    // 条件：accept 返回负数（出错或被中断）
    if (client_fd < 0) {
      if (stop_flag->load()) break;  // 若此刻正好收到停止信号，就借机退出主循环
      continue;                      // 否则当作偶发错误，回到循环重新等连接
    }
    LOG(INFO) << "上位机已连接";

    std::string buffer;   // 累积收到的原始字节，等凑齐一整行再处理（粘包/半包都靠它兜着）
    char chunk[256];      // 每次 recv 的临时接收缓冲区
    // 内层循环：从这个连接持续读数据，按 '\n' 切分成一行行命令
    // 条件：同样受 stop_flag 控制，收到停止信号就断开
    while (!stop_flag->load()) {
      // recv 从连接读一批数据到 chunk，返回实际读到的字节数（ssize_t 是带符号的长度类型）
      ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
      // 条件：返回 0 表示对方正常关闭连接，返回负数表示出错——两种都意味着这条连接结束了
      if (n <= 0) {
        LOG(INFO) << "上位机断开连接";
        break;
      }
      buffer.append(chunk, n);  // 把这批新字节追加到累积缓冲区末尾

      size_t pos;
      // 只要缓冲区里还能找到换行符，就说明攒够了至少一整行，循环把它们一条条切出来
      // npos 是 std::string 的“没找到”返回值；find != npos 即“找到了换行符”
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);  // 取出换行符前的一整行
        buffer.erase(0, pos + 1);                  // 把这行连同换行符从缓冲区删掉，留下剩余部分
        // 去掉可能的 '\r'（Windows 换行是 "\r\n"，切完 '\n' 后行尾可能还留个 '\r'）
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;  // 空行（比如客户端多敲了个回车）直接跳过

        std::string reply = HandleLine(line);              // 交给上面的解析函数处理，拿到应答
        send(client_fd, reply.c_str(), reply.size(), 0);   // 把应答原样发回给上位机
      }
    }
    close(client_fd);  // 这条连接结束，关掉它的 fd，再回外层循环等下一个客户端
  }
  close(listen_fd);  // 整个服务退出前，关掉监听用的 fd
  LOG(INFO) << "socket 服务器退出";
}

}  // namespace demo
