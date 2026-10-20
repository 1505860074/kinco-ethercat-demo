/*
 * kinco_driver.cpp —— Kinco 驱动器 EtherCAT 抽象的实现（学习精简版）
 *
 * 抽取自原项目 controller/source/ethercat/motor/ecat_kinco_driver.cpp，
 * 只保留 PP（轮廓位置）模式这一条主线。
 */
#include "kinco_driver.h"

#include <cstring>

#include "config.h"
#include "log.h"

namespace demo {

// ============================================================
// PDO 映射表（原样保留自原项目，这是真实的 Kinco FD PDO 定义）
// ============================================================
// 每一行 {对象索引, 子索引, 位宽}，描述 PDO 里逐个字段。
// 顺序必须和上面 RxPDO_1601_t / TxPDO_1A01_t 里字段顺序一致。
static ec_pdo_entry_info_t pp_pdo_entries[] = {
    // ---- RxPDO 0x1601：主站 → 驱动器 ----
    {0x607a, 0x00, 32},  // Target_Position
    {0x6040, 0x00, 16},  // Control_Word
    {0x6060, 0x00, 8},   // Operation_mode
    {0x6083, 0x00, 32},  // Profile_Acce
    {0x6084, 0x00, 32},  // Profile_Dece
    {0x607f, 0x00, 32},  // Max_profile_velocity
    {0x6081, 0x00, 32},  // Target_velocity
    {0x6073, 0x00, 16},  // Maximum_current
    // ---- TxPDO 0x1a01：驱动器 → 主站 ----
    {0x6064, 0x00, 32},  // Actual_position
    {0x6041, 0x00, 16},  // Status_word
    {0x6061, 0x00, 8},   // mode_of_operation_display
    {0x606b, 0x00, 32},  // Target_Velocity
    {0x606c, 0x00, 32},  // Actual_Velocity
    {0x603f, 0x00, 16},  // Error_code
    {0x6078, 0x00, 16},  // Actual_current
};

// 把上面 15 个条目分成两个 PDO：前 8 个是 RxPDO(0x1601)，后 7 个是 TxPDO(0x1a01)
static ec_pdo_info_t pp_pdos[] = {
    {0x1601, 8, pp_pdo_entries + 0},  // RxPDO
    {0x1a01, 7, pp_pdo_entries + 8},  // TxPDO
};

// Sync Manager 配置：告诉从站哪个 SM 通道对应输出、哪个对应输入。
static ec_sync_info_t pp_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_ENABLE},
    {1, EC_DIR_INPUT, 0, NULL, EC_WD_ENABLE},
    {2, EC_DIR_OUTPUT, 1, pp_pdos + 0, EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, pp_pdos + 1, EC_WD_ENABLE},
    {0xff}};

// ============================================================
// ConfigureOnBus：登记从站 + 配 PDO + 注册 domain 字段偏移
// ============================================================
int KincoDriver::ConfigureOnBus(ec_master_t* master, ec_domain_t* domain) {
  LOG(INFO) << "配置 Kinco 从站: position=" << kKincoPosition << std::hex
            << " vendor=0x" << kKincoVendorID << " product=0x"
            << kKincoProductCode;

  // 1) 在总线上按 (alias=0, position, vendor, product) 找到并登记这个从站
  slave_config_ = ecrt_master_slave_config(master, 0, kKincoPosition,
                                            kKincoVendorID, kKincoProductCode);
  if (!slave_config_) {
    LOG(ERROR) << "ecrt_master_slave_config 失败";
    return -1;
  }

  // 2) 给从站套用上面定义的 PP 模式 PDO 映射
  if (ecrt_slave_config_pdos(slave_config_, EC_END, pp_syncs)) {
    LOG(ERROR) << "ecrt_slave_config_pdos 失败";
    return -1;
  }

  // 3) 把每个 PDO 字段注册到 domain，并让主站把它在共享内存里的“字节偏移”
  //    回填到 off_1601_ / off_1a01_ 里。之后读写就靠这些偏移。
  //    这里用一张“注册表”，每一项 = {index, subindex, 存偏移的地址}。
  ec_pdo_entry_reg_t regs[] = {
      // RxPDO 字段
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x607a, 0x00,
       &off_1601_.Target_Position},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6040, 0x00,
       &off_1601_.Control_Word},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6060, 0x00,
       &off_1601_.Operation_mode},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6083, 0x00,
       &off_1601_.Profile_Acce},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6084, 0x00,
       &off_1601_.Profile_Dece},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x607f, 0x00,
       &off_1601_.Max_profile_velocity},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6081, 0x00,
       &off_1601_.Target_velocity},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6073, 0x00,
       &off_1601_.Maximum_current},
      // TxPDO 字段
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6064, 0x00,
       &off_1a01_.Actual_position},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6041, 0x00,
       &off_1a01_.Status_word},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6061, 0x00,
       &off_1a01_.mode_of_operation_display},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x606b, 0x00,
       &off_1a01_.Target_Velocity},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x606c, 0x00,
       &off_1a01_.Actual_Velocity},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x603f, 0x00,
       &off_1a01_.Error_code},
      {0, kKincoPosition, kKincoVendorID, kKincoProductCode, 0x6078, 0x00,
       &off_1a01_.Actual_current},
      {}  // 结束哨兵：全 0 表示注册表到此结束
  };

  if (ecrt_domain_reg_pdo_entry_list(domain, regs)) {
    LOG(ERROR) << "ecrt_domain_reg_pdo_entry_list 失败";
    return -1;
  }

  LOG(INFO) << "Kinco 从站配置完成";
  return 0;
}

// ============================================================
// ReadInputs：把驱动器回读的 TxPDO 从共享内存解析进 ecat_data
// ============================================================
// EC_READ_* 是 IgH 提供的宏，按小端从 (domain_data + 偏移) 处读出对应位宽的值。
void KincoDriver::ReadInputs(uint8_t* domain_data) {
  ecat_data.mI_1a01.Actual_position =
      EC_READ_S32(domain_data + off_1a01_.Actual_position);
  ecat_data.mI_1a01.Status_word =
      EC_READ_U16(domain_data + off_1a01_.Status_word);
  ecat_data.mI_1a01.mode_of_operation_display =
      EC_READ_S8(domain_data + off_1a01_.mode_of_operation_display);
  ecat_data.mI_1a01.Actual_Velocity =
      EC_READ_S32(domain_data + off_1a01_.Actual_Velocity);
  ecat_data.mI_1a01.Error_code =
      EC_READ_U16(domain_data + off_1a01_.Error_code);
  ecat_data.mI_1a01.Actual_current =
      EC_READ_S16(domain_data + off_1a01_.Actual_current);
}

// ============================================================
// WriteOutputs：把要下发的 RxPDO 写进共享内存
// ============================================================
void KincoDriver::WriteOutputs(uint8_t* domain_data) {
  EC_WRITE_S32(domain_data + off_1601_.Target_Position,
               ecat_data.mO_1601.Target_Position);
  EC_WRITE_U16(domain_data + off_1601_.Control_Word,
               ecat_data.mO_1601.Control_Word);
  EC_WRITE_S8(domain_data + off_1601_.Operation_mode,
              ecat_data.mO_1601.Operation_mode);
  EC_WRITE_U32(domain_data + off_1601_.Profile_Acce,
               ecat_data.mO_1601.Profile_Acce);
  EC_WRITE_U32(domain_data + off_1601_.Profile_Dece,
               ecat_data.mO_1601.Profile_Dece);
  EC_WRITE_U32(domain_data + off_1601_.Max_profile_velocity,
               ecat_data.mO_1601.Max_profile_velocity);
  EC_WRITE_S32(domain_data + off_1601_.Target_velocity,
               ecat_data.mO_1601.Target_velocity);
  EC_WRITE_S16(domain_data + off_1601_.Maximum_current,
               ecat_data.mO_1601.Maximum_current);
}

// ============================================================
// UpdateEnableStateMachine：CiA402 标准使能握手
// ============================================================
// 目标：把驱动器从“断电/未使能”一步步推进到“运行使能(Operation Enabled)”。
// 做法就是看当前状态字(Status_word)在哪一步，就下发对应的控制字(Control_Word)：
//   0x06 Shutdown        → 进入 Ready to switch on
//   0x07 Switch on       → 进入 Switched on（已上电）
//   0x0F Enable operation→ 进入 Operation enabled（可以动了）
//   0x86 Fault reset     → 有故障时先复位
// 这就是原项目 GetControlWord 的精简版。
bool KincoDriver::UpdateEnableStateMachine() {
  uint16_t sw = ecat_data.mI_1a01.Status_word;
  uint16_t& cw = ecat_data.mO_1601.Control_Word;

  if (sw & (1 << STATUSWORD_FAULT_BIT)) {  // 故障
    cw = CONTROLWORD_FAULT_RESET;          // 先复位
    enabled_ = false;
    return false;
  }
  if (sw & (1 << STATUSWORD_OPERATION_ENABLE_BIT)) {  // 运行使能
    // 已经使能：保持使能运行（运动子状态机会在此基础上叠加 bit4）
    if (!enabled_) {
      LOG(INFO) << "驱动器已进入【运行使能】状态";
    }
    enabled_ = true;
    return true;
  }
  if (sw & (1 << STATUSWORD_SWITCHED_ON_BIT)) {  // 已上电
    cw = CONTROLWORD_ENABLE_OPERATION;           // 使能运行
    return false;
  }
  if (sw & (1 << STATUSWORD_READY_TO_SWITCH_ON_BIT)) {  // 已准备好上电
    cw = CONTROLWORD_SWITCH_ON;                         // 上电
    return false;
  }
  cw = CONTROLWORD_SHUTDOWN;  // 其它情况 → Shutdown，从头开始
  return false;
}

// ============================================================
// StartRelativeMove：发起一次相对角度运动
// ============================================================
void KincoDriver::StartRelativeMove(int32_t delta_counts) {
  if (!enabled_) {
    LOG(WARNING) << "驱动器尚未使能，忽略本次运动指令";
    return;
  }
  // PP 模式：目标位置 = 当前实际位置 + 相对增量
  int32_t target = ecat_data.mI_1a01.Actual_position + delta_counts;

  ecat_data.mO_1601.Operation_mode = PositionMode;  // 0x6060 = 1 (PP)
  ecat_data.mO_1601.Target_Position = target;       // 0x607a
  ecat_data.mO_1601.Profile_Acce = kProfileAcce;
  ecat_data.mO_1601.Profile_Dece = kProfileDece;
  ecat_data.mO_1601.Max_profile_velocity = kProfileVelocity;
  ecat_data.mO_1601.Maximum_current = kMaxCurrent;

  move_state_ = MoveState::kSetSetpoint;
  LOG(INFO) << "发起相对运动: delta=" << delta_counts
            << " counts, 从 " << ecat_data.mI_1a01.Actual_position << " → "
            << target;
}

// ============================================================
// UpdateMotion：PP 模式的“新设点(new set-point)”握手子状态机
// ============================================================
// PP 模式让电机真正动起来，需要一次“上升沿握手”：
//   1) 写好目标位置后，把控制字 bit4(new set-point) 从 0 抬到 1 (0x0F → 0x1F)
//   2) 驱动器收到后，把状态字 bit12(set-point ack) 置 1 表示“我接受了”
//   3) 主站再把 bit4 落回 0 (0x1F → 0x0F)，握手完成，驱动器开始走轨迹
//   4) 电机到位后，状态字 bit10(target reached) 置 1，本次运动结束
void KincoDriver::UpdateMotion() {
  uint16_t sw = ecat_data.mI_1a01.Status_word;
  uint16_t& cw = ecat_data.mO_1601.Control_Word;

  switch (move_state_) {
    case MoveState::kIdle:
      break;

    case MoveState::kSetSetpoint:
      cw |= (1 << CONTROLWORD_NEW_SETPOINT_BIT);  // 抬起 bit4：0x0F → 0x1F
      move_state_ = MoveState::kWaitAck;
      break;

    case MoveState::kWaitAck:
      if (sw & (1 << STATUSWORD_SETPOINT_ACK_BIT)) {  // 状态字 bit12 = set-point ack
        cw &= ~(1 << CONTROLWORD_NEW_SETPOINT_BIT);   // 落回 bit4：0x1F → 0x0F，握手完成
        move_state_ = MoveState::kWaitReached;
      }
      break;

    case MoveState::kWaitReached:
      if (sw & (1 << STATUSWORD_TARGET_REACHED_BIT)) {  // 状态字 bit10 = target reached
        LOG(INFO) << "运动完成，到达目标位置: "
                  << ecat_data.mI_1a01.Actual_position;
        move_state_ = MoveState::kIdle;
      }
      break;
  }
}

}  // namespace demo
