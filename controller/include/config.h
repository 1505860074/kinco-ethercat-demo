/*
 * config.h —— 全项目的可调参数集中放这里
 *
 * 学习项目里我们不引入 yaml-cpp（原项目用 yaml 配置），为了极简，
 * 把所有常量写成一个头文件里的 constexpr。真机上要调参数，改这里重新编译即可。
 *
 * 对应原项目：controller/box_aligner_config.yaml + sorting_stage_config.h
 */
#pragma once

#include <cstdint>

namespace demo {

// ---------- socket 上位机通信 ----------
// 下位机监听的 TCP 端口，上位机 send_command.py 连这个端口。
// 原项目理料台监听 5051，这里沿用，方便你对照。
constexpr int kListenPort = 5051;

// ---------- EtherCAT 周期 ----------
// 每个总线周期的时长（纳秒）。原项目是 1ms 硬实时（Xenomai），
// 我们剔除了 Xenomai，用普通 Linux 线程 + nanosleep 跑 1ms 循环。
// PP（轮廓位置）模式不需要硬实时，1ms 的普通循环足够让电机精确转到角度。
constexpr uint32_t kCycleNs = 1'000'000;  // 1 ms

// ---------- Kinco 从站身份 ----------
// 这两个值来自原项目 ecat_kinco_driver.cpp，是禾川(Kinco) FD 驱动器的
// 厂商码和产品码。EtherCAT 主站靠 (alias, position, vendor_id, product_code)
// 在总线上认出这个从站。
constexpr uint32_t kKincoVendorID = 0x00681168;
constexpr uint32_t kKincoProductCode = 0x00464445;

// 从站在总线上的位置（0 表示总线上第一个从站）。
// 我们用“单独测试总线”：电脑网口 → 一个 Kinco → 电机，所以就是 0。
constexpr int kKincoPosition = 0;

// ---------- 角度 → 编码器计数 换算 ----------
// 关键参数！Kinco 驱动器“目标位置”(对象 0x607a) 的单位是“编码器计数”，
// 转一整圈对应多少计数，取决于这台驱动器自身的电子齿轮比/编码器分辨率设置。
// 10000 是 Kinco FD 系列常见的默认值（每圈 10000 计数）。
// !!! 真机上请按你那台驱动器的实际设置修改，否则“转 90 度”不准 !!!
constexpr int32_t kCountsPerRev = 10000;

// 由角度换算目标计数：counts = angle_deg / 360 * kCountsPerRev
inline int32_t DegreeToCounts(double angle_deg) {
  return static_cast<int32_t>(angle_deg / 360.0 * kCountsPerRev);
}

// ---------- PP（轮廓位置）运动参数 ----------
// 这些值会通过 PDO 每周期发给驱动器，控制这次运动的速度和加减速。
// 单位同样是驱动器内部单位，真机上可按手感微调。
constexpr uint32_t kProfileVelocity = 200;   // 最大轮廓速度 (0x607f)
constexpr uint32_t kProfileAcce = 1000;      // 轮廓加速度 (0x6083)
constexpr uint32_t kProfileDece = 1000;      // 轮廓减速度 (0x6084)
constexpr int16_t kMaxCurrent = 1000;        // 最大电流限制 (0x6073)

}  // namespace demo
