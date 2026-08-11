/*
 * main.cpp —— 下位机入口（学习精简版）
 *
 * 对应原项目 controller/source/sortingstage_main.cpp。
 * 职责：初始化 EtherCAT 主站 → 起一个 socket 监听线程 → 主线程跑周期循环。
 *
 * 整条链路（本 demo 的核心，一定要看懂）：
 *   上位机 send_command.py
 *     --TCP文本"MOVE 90"-->  socket_server(监听线程)
 *     --RequestRelativeMove--> EcatMaster(周期循环, 主线程)
 *     --CiA402使能 + PP设点 + PDO--> Kinco 驱动器 --> 电机转 90 度
 */
// —— 标准库依赖（都跟"多线程 + 优雅退出"这条主线相关，故简单说明）——
#include <atomic>   // std::atomic：原子类型。让 g_stop 这个标志能被"信号 + 主线程 + socket 线程"三方安全读写，不会读到写一半的脏值
#include <csignal>  // std::signal：注册操作系统信号（如 Ctrl+C）的处理函数，用来接管退出流程
#include <thread>   // std::thread：开一条新线程，本文件用它把 socket 监听单独放到一条线程上跑

// —— 项目内部依赖 ——
#include "config.h"         // 全局配置（周期、从站参数等常量），被下面各模块共用
#include "ecat_master.h"    // demo::EcatMaster：EtherCAT 主站，负责初始化总线并跑 1ms 周期循环驱动电机
#include "log.h"            // LOG(INFO)/LOG(ERROR) 日志宏，统一打印格式
#include "socket_server.h"  // demo::SocketServer：TCP 服务端，收上位机(send_command.py)发来的文本命令

// 全局停止标志：Ctrl+C 时置 true，两个循环都会优雅退出。
// 用 atomic 是因为它会被信号处理函数、主线程周期循环、socket 线程同时访问——必须保证并发读写安全。
// static：给它"内部链接"，把作用域限制在本文件内，避免和别处的同名全局符号冲突。
static std::atomic<bool> g_stop{false};   

// 信号处理函数：进程收到 SIGINT/SIGTERM 时被操作系统回调。
// 这里只做一件极简单的事——把停止标志置真；真正的收尾交给两个循环自己去发现并退出。
// （信号处理函数里能安全做的操作有限，给 atomic 赋值属于安全操作，所以刻意写得这么"轻"。）
// 参数 sig 是信号编号，本 demo 用不到，故写成匿名注释 /*sig*/ 表示"故意忽略"。
// .store(true)：atomic 的写操作，等价于把值原子地设为 true。
static void OnSignal(int /*sig*/) { g_stop.store(true); }

// 程序入口：把"注册退出信号 → 初始化主站 → 起监听线程 → 跑周期循环 → 收尾"这条启动流程串起来。
int main() {
  // 捕获 Ctrl+C / kill，方便干净退出
  std::signal(SIGINT, OnSignal);   // SIGINT：终端按 Ctrl+C 发的中断信号，交给 OnSignal 处理
  std::signal(SIGTERM, OnSignal);  // SIGTERM：kill 命令默认发的终止信号，同样走 OnSignal，实现优雅停机

  LOG(INFO) << "===== Kinco EtherCAT 学习 demo 启动 =====";  // 打一条启动横幅，方便日志里定位本次运行的起点

  // 1) 初始化 EtherCAT 主站（请求主站、配从站、激活）
  demo::EcatMaster master;       // 在栈上创建主站对象；它内部持有总线句柄、PDO 内存等资源
  // 条件：Init() 返回非 0 代表初始化失败（约定 0 为成功）。必须在 RunForever 之前调用。
  if (master.Init() != 0) {
    // 进入此分支：总线没起来（多半是没加载 IgH 内核模块或没接从站），无法继续，直接退出。
    LOG(ERROR) << "主站初始化失败，退出。"
                  "（真机需已加载 IgH 内核模块并接上 Kinco 从站）";
    return 1;  // 返回非 0 退出码，告诉外部脚本/shell"本次运行失败"
  }

  // 2) 起 socket 监听线程：负责收上位机命令
  demo::SocketServer server(&master);  // 把主站指针交给 server，收到命令后它才能调用主站去发运动指令
  // 新开一条线程专门跑 socket 监听循环，避免它阻塞主线程的实时周期循环。
  // [&]：lambda 的"按引用捕获"，让这个匿名函数能直接用到外面的 server 和 g_stop。
  std::thread socket_thread(
      [&]() { server.RunForever(&g_stop); });  // 线程体：循环收命令，直到 g_stop 变 true 才返回

  // 3) 主线程跑 EtherCAT 周期循环（1ms 一个周期），直到收到停止信号
  // 这是本 demo 的实时主循环：每 1ms 收发一次 PDO、推进 CiA402 状态机。占用主线程直到 g_stop 被置真。
  master.RunForever(&g_stop);

  // 4) 收尾
  // 走到这里说明主循环已因 g_stop 退出；此时 socket 线程也会陆续发现标志退出。
  // joinable()：判断线程是否还需要 join（未被 join 过且确实启动了）；join()：阻塞等它彻底结束，回收线程资源，防止悬挂。
  if (socket_thread.joinable()) socket_thread.join();
  LOG(INFO) << "===== demo 退出 =====";  // 打一条退出横幅，标记本次运行干净收尾
  return 0;  // 返回 0：正常退出
}
