/*
 * ecat_master.h —— IgH EtherCAT 主站生命周期 + 周期循环（学习精简版）
 *
 * 抽取并精简自原项目 controller/source/ethercat/ecat_manager.cpp。
 * 原项目在这里管理一整条总线上的所有从站（IO、滚筒、夹爪、机械臂……），
 * 我们只留一个 Kinco 驱动器，把“主站怎么起、周期里怎么收发”这条主线讲清楚。
 *
 * 剔除了 Xenomai 实时框架：原项目用 alchemy 实时任务跑 1ms 硬实时，
 * 我们用普通 std::thread + nanosleep 跑 1ms 软循环（PP 模式够用）。
 */
#pragma once

#include <atomic>

#include "ecrt.h"
#include "kinco_driver.h"

namespace demo {

class EcatMaster {
 public:
  EcatMaster() = default;
  ~EcatMaster();

  // 初始化：请求主站 → 建 domain → 配 Kinco 从站 → 激活主站 → 拿共享内存指针。
  // 返回 0 成功。必须在 RunForever 之前调用。
  int Init();

  // 阻塞式跑周期循环，直到 stop_flag 变为 true。每 kCycleNs 一个周期。
  void RunForever(std::atomic<bool>* stop_flag);

  // 由 socket 线程调用：请求相对转动 delta_counts 个编码器计数（线程安全）。
  void RequestRelativeMove(int32_t delta_counts);

  // 供外部（如 socket 线程回读状态）读取最近一次的实际位置/状态字（线程安全）。
  int32_t last_actual_position() const { return last_actual_position_.load(); }
  uint16_t last_status_word() const { return last_status_word_.load(); }

 private:
  // 跑一个总线周期：收 → 处理 → 读状态 → 推进状态机 → 写指令 → 排队 → 发。
  void SpinOnce();

  ec_master_t* master_ = nullptr;
  ec_domain_t* domain_ = nullptr;
  uint8_t* domain_data_ = nullptr;

  KincoDriver kinco_;

  // socket 线程 → 周期循环 传递运动指令的“信箱”（无锁原子变量）
  std::atomic<int32_t> pending_delta_{0};
  std::atomic<bool> has_pending_{false};

  // 周期循环 → 外部 回传的最新状态
  std::atomic<int32_t> last_actual_position_{0};
  std::atomic<uint16_t> last_status_word_{0};
};

}  // namespace demo
