/*
 * ecat_master.cpp —— IgH 主站生命周期 + 周期循环 实现（学习精简版）
 */
#include "ecat_master.h"

#include <ctime>

#include "config.h"
#include "log.h"

namespace demo {

EcatMaster::~EcatMaster() {
  if (master_) {
    ecrt_release_master(master_);  // 归还主站
    master_ = nullptr;
  }
}

// ============================================================
// Init：主站启动的固定四步（对照原项目 ecat_manager 注释里的 ①②③④）
// ============================================================
int EcatMaster::Init() {
  // ① 请求 0 号主站。真机上这一步要求内核里已经加载了 IgH 的 ec_master
  //    内核模块、并存在 /dev/EtherCAT 设备节点，否则会返回失败。
  master_ = ecrt_request_master(0);
  if (!master_) {
    LOG(ERROR) << "ecrt_request_master(0) 失败：确认已加载 IgH 内核模块、"
                  "且有权限访问 /dev/EtherCAT";
    return -1;
  }

  // ② 创建一个 domain。domain 是“一块把若干 PDO 字段打包在一起、
  //    每周期整体收发的共享内存区”。我们只有一个 Kinco，就用一个 domain。
  domain_ = ecrt_master_create_domain(master_);
  if (!domain_) {
    LOG(ERROR) << "ecrt_master_create_domain 失败";
    return -1;
  }

  // ③ 在总线上登记 Kinco 从站、配置它的 PDO、把 PDO 字段注册进 domain。
  if (kinco_.ConfigureOnBus(master_, domain_) != 0) {
    LOG(ERROR) << "配置 Kinco 从站失败";
    return -1;
  }

  // ④ 激活主站。激活之后总线开始运转，PDO 布局也就固定了。
  if (ecrt_master_activate(master_)) {
    LOG(ERROR) << "ecrt_master_activate 失败";
    return -1;
  }

  // 拿到 domain 的共享内存首地址。之后每周期就靠 domain_data_ + 偏移 读写 PDO。
  domain_data_ = ecrt_domain_data(domain_);
  if (!domain_data_) {
    LOG(ERROR) << "ecrt_domain_data 失败";
    return -1;
  }

  LOG(INFO) << "EtherCAT 主站初始化完成，开始周期循环";
  return 0;
}

// ============================================================
// SpinOnce：一个总线周期做的事（顺序照搬原项目 ecat_manager.cpp:739）
// ============================================================
void EcatMaster::SpinOnce() {
  // 1) 接收上一周期总线上回来的数据帧
  ecrt_master_receive(master_);
  // 2) 处理 domain：把收到的数据搬进 domain 共享内存
  ecrt_domain_process(domain_);

  // 3) 从共享内存把驱动器回读的状态解析进 kinco_.ecat_data
  kinco_.ReadInputs(domain_data_);

  // 把最新状态发布给外部（socket 线程读回用）
  last_actual_position_.store(kinco_.actual_position());
  last_status_word_.store(kinco_.status_word());

  // 4) 推进 CiA402 使能状态机（把驱动器一步步推到“运行使能”）
  bool enabled = kinco_.UpdateEnableStateMachine();

  // 5) 如果已使能、当前空闲、且信箱里有新的运动请求，就取出来发起运动
  if (enabled && kinco_.MoveFinished() && has_pending_.load()) {
    int32_t delta = pending_delta_.exchange(0);
    has_pending_.store(false);
    kinco_.StartRelativeMove(delta);
  }

  // 6) 推进 PP 运动子状态机（新设点 bit4 握手 / 判断是否到位）
  kinco_.UpdateMotion();

  // 7) 把要下发的指令写进共享内存
  kinco_.WriteOutputs(domain_data_);
  // 8) 把 domain 排入发送队列
  ecrt_domain_queue(domain_);
  // 9) 真正把这一周期的数据帧发到总线上
  ecrt_master_send(master_);
}

// ============================================================
// RunForever：软实时周期循环（替代原项目的 Xenomai 实时任务）
// ============================================================
void EcatMaster::RunForever(std::atomic<bool>* stop_flag) {
  // 用 CLOCK_MONOTONIC 绝对时间递推每个周期的唤醒点，避免累积漂移。
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);

  while (!stop_flag->load()) {
    SpinOnce();

    // 计算下一个周期的绝对唤醒时刻 = 当前唤醒点 + kCycleNs
    next.tv_nsec += kCycleNs;
    while (next.tv_nsec >= 1'000'000'000L) {
      next.tv_nsec -= 1'000'000'000L;
      next.tv_sec += 1;
    }
    // 睡到那个绝对时刻（TIMER_ABSTIME）。原项目用 Xenomai 保证硬实时抖动，
    // 我们用普通 clock_nanosleep，PP 模式对抖动不敏感，够用。
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
  }
  LOG(INFO) << "周期循环收到停止信号，退出";
}

// ============================================================
// RequestRelativeMove：socket 线程往"信箱"投运动请求（跨线程交接点）
// ============================================================
// 由 socket 线程调用（见 socket_server.cpp 的 HandleLine），本函数只写两个原子
// 变量就立刻返回——不碰 EtherCAT、不等电机，所以绝不会拖慢调用方。真正发起运动
// 的是主线程的周期循环，它在 SpinOnce 第 5 步来取信（见本文件 has_pending_ 处）。
// 这样分工是为了让 1ms 周期循环独占硬件：socket 线程会长时间阻塞在 recv 上，
// 若让它直接操作共享内存，两条线程就会数据竞争、周期也会被网络拖垮。
void EcatMaster::RequestRelativeMove(int32_t delta_counts) {
  // 先放数据：这次要相对转多少个编码器计数。store() 是原子写，
  // 保证周期循环那条线程不会读到"写了一半"的坏值。
  pending_delta_.store(delta_counts);
  // 再竖旗子：通知周期循环"信箱里有新请求了"。
  // 顺序不能颠倒——取信方是"看到旗子才去读数据"，先竖旗会留下一个
  // "旗已立、数据还是旧值"的窗口，周期循环恰好这时来取就会执行上一次的角度。
  has_pending_.store(true);
}

}  // namespace demo
