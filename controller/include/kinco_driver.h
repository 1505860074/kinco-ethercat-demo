/*
 * kinco_driver.h —— Kinco(禾川) 驱动器的 EtherCAT 抽象（学习精简版）
 *
 * 抽取并精简自原项目：
 *   controller/include/ethercat/motor/ecat_kinco_driver.h
 *   controller/source/ethercat/motor/ecat_kinco_driver.cpp
 *
 * 这一层的职责：把“让电机转到某个目标位置”这件事，翻译成 EtherCAT 总线上
 * 一来一回的 PDO 数据 + 一套标准的 CiA402 驱动器使能握手。
 *
 * 只保留 PP（轮廓位置 Profile Position）模式，删掉了原项目的 CSP/CSV 模式、
 * SDO 参数下发、多电机等与本 demo 无关的内容。
 */
#pragma once

#include <cstdint>

#include "ecrt.h"

namespace demo {

// ============================================================
// 一、PDO 数据结构（原样保留自原项目，这是真实的 Kinco FD 协议）
// ============================================================
//
// PDO(Process Data Object) 是 EtherCAT 每个周期高速交换的“过程数据”。
// RxPDO = 主站→驱动器（我们下发的指令）；TxPDO = 驱动器→主站（回读的状态）。
// “步科所有电机无论位置模式还是速度模式，都用同一组 PDO。”

// RxPDO 0x1601：主站每周期发给驱动器的指令
typedef struct {
  int32_t Target_Position;        // 0x607a 目标位置（编码器计数）
  uint16_t Control_Word;          // 0x6040 控制字（驱动 CiA402 状态机）
  int8_t Operation_mode;          // 0x6060 运行模式（1=PP 轮廓位置）
  uint32_t Profile_Acce;          // 0x6083 轮廓加速度
  uint32_t Profile_Dece;          // 0x6084 轮廓减速度
  uint32_t Max_profile_velocity;  // 0x607f 最大轮廓速度
  int32_t Target_velocity;        // 0x6081 目标速度
  int16_t Maximum_current;        // 0x6073 最大电流
} RxPDO_1601_t;

// TxPDO 0x1a01：驱动器每周期回读给主站的状态
typedef struct {
  int32_t Actual_position;            // 0x6064 实际位置
  uint16_t Status_word;               // 0x6041 状态字（反映 CiA402 状态）
  int8_t mode_of_operation_display;   // 0x6061 当前实际运行模式
  int32_t Target_Velocity;            // 0x606b
  int32_t Actual_Velocity;            // 0x606c 实际速度
  uint16_t Error_code;                // 0x603f 错误码
  int16_t Actual_current;             // 0x6078 实际电流
} TxPDO_1A01_t;

// 把收发两组数据打包在一起，作为“当前这台驱动器的镜像”
typedef struct {
  RxPDO_1601_t mO_1601;  // Output：我们要写下去的
  TxPDO_1A01_t mI_1a01;  // Input：读回来的
} FD_Drive_dev_t;

// PDO 各字段在 domain 内存里的“字节偏移”。EtherCAT 主站激活后，
// 会把每个 PDO 字段在共享内存 domain_data 里的位置填进这个结构，
// 之后我们靠这些偏移用 EC_READ_*/EC_WRITE_* 读写对应字段。
typedef struct {
  uint32_t Target_Position;
  uint32_t Control_Word;
  uint32_t Operation_mode;
  uint32_t Profile_Acce;
  uint32_t Profile_Dece;
  uint32_t Max_profile_velocity;
  uint32_t Target_velocity;
  uint32_t Maximum_current;
} rx_1601_off_t;

typedef struct {
  uint32_t Actual_position;
  uint32_t Status_word;
  uint32_t mode_of_operation_display;
  uint32_t Target_Velocity;
  uint32_t Actual_Velocity;
  uint32_t Error_code;
  uint32_t Actual_current;
} tx_1a01_off_t;

// ============================================================
// 二、CiA402 状态字 / 控制字常量（用 constexpr 全局常量，不用 #define 宏）
// ============================================================
// CiA402 是伺服驱动器的国际标准状态机。主站通过“控制字(0x6040)”下命令，
// 驱动器通过“状态字(0x6041)”回报自己处在哪个状态。判断状态字某一位、
// 置位 / 清位都直接用原始位运算：(1 << n)、val & (1 << n)、&= ~(1 << n)。

// 状态字(0x6041)各位
constexpr int STATUSWORD_READY_TO_SWITCH_ON_BIT = 0;  // 已准备好上电
constexpr int STATUSWORD_SWITCHED_ON_BIT = 1;         // 已上电
constexpr int STATUSWORD_OPERATION_ENABLE_BIT = 2;    // 运行使能（可以真正动了）
constexpr int STATUSWORD_FAULT_BIT = 3;               // 故障标志
constexpr int STATUSWORD_TARGET_REACHED_BIT = 10;     // 已到达目标位置
constexpr int STATUSWORD_SETPOINT_ACK_BIT = 12;       // 设点已被驱动器确认（PP 握手用）

// 控制字(0x6040)命令值
constexpr uint16_t CONTROLWORD_SHUTDOWN = 0x06;          // 从头开始 → Ready to switch on
constexpr uint16_t CONTROLWORD_SWITCH_ON = 0x07;         // 上电 → Switched on
constexpr uint16_t CONTROLWORD_ENABLE_OPERATION = 0x0F;  // 使能运行 → Operation enabled
constexpr uint16_t CONTROLWORD_FAULT_RESET = 0x86;       // 故障复位

// 控制字 bit4 = new set-point（PP 模式发起新运动的握手位）
constexpr int CONTROLWORD_NEW_SETPOINT_BIT = 4;

// 运行模式枚举（对象 0x6060）。本 demo 只用 PP。
enum OperationMode {
  PositionMode = 1,  // PP 轮廓位置模式：给一个目标位置，驱动器自己规划轨迹走过去
};

// ============================================================
// 三、驱动器类
// ============================================================
class KincoDriver {
 public:
  KincoDriver() = default;
  ~KincoDriver() = default;

  // 在总线上登记这个从站 + 配置它的 PDO + 把 PDO 字段注册进 domain。
  // 必须在 ecrt_master_activate 之前调用。
  // 返回 0 成功，非 0 失败。
  int ConfigureOnBus(ec_master_t* master, ec_domain_t* domain);

  // 每周期第一步：从 domain 共享内存里把驱动器回读的 TxPDO 解析进 ecat_data。
  void ReadInputs(uint8_t* domain_data);

  // 每周期最后一步：把 ecat_data 里要下发的 RxPDO 写进 domain 共享内存。
  void WriteOutputs(uint8_t* domain_data);

  // 根据当前状态字，算出这一周期应该下发的控制字，推进 CiA402 使能流程。
  // 返回 true 表示驱动器已处于“运行使能”状态（可以接收运动指令了）。
  bool UpdateEnableStateMachine();

  // 发起一次相对运动：从“当前实际位置”相对转动 delta_counts 个计数。
  void StartRelativeMove(int32_t delta_counts);

  // 是否已经走完上一次运动（到达目标）。
  bool MoveFinished() const { return move_state_ == MoveState::kIdle; }

  // 每周期推进一次“运动子状态机”（PP 模式的新设点 bit4 握手）。
  void UpdateMotion();

  // 供外部读状态用
  int32_t actual_position() const { return ecat_data.mI_1a01.Actual_position; }
  uint16_t status_word() const { return ecat_data.mI_1a01.Status_word; }
  uint16_t error_code() const { return ecat_data.mI_1a01.Error_code; }

  FD_Drive_dev_t ecat_data{};  // 这台驱动器的收发数据镜像

 private:
  // PP 模式运动的子状态机：发目标 → 抬 bit4 → 等确认 → 落 bit4 → 等到位
  enum class MoveState {
    kIdle,           // 空闲，没有运动
    kSetSetpoint,    // 已写好目标位置，准备抬起 bit4
    kWaitAck,        // 已抬 bit4，等驱动器把 setpoint-ack(bit12) 置起
    kWaitReached,    // 已落 bit4，等 target-reached(bit10)
  };

  ec_slave_config_t* slave_config_ = nullptr;
  rx_1601_off_t off_1601_{};  // RxPDO 各字段字节偏移
  tx_1a01_off_t off_1a01_{};  // TxPDO 各字段字节偏移

  MoveState move_state_ = MoveState::kIdle;
  bool enabled_ = false;  // 是否已使能完成
};

}  // namespace demo
