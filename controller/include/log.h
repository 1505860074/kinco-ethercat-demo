/*
 * log.h —— 极简日志宏，替代原项目的 glog
 *
 * 原项目用 Google 的 glog 库（LOG(INFO) << ...），依赖较重。
 * 学习项目为了“依赖最小”，自己用十几行代码模仿出同样的用法：
 *
 *     LOG(INFO)  << "hello " << 123;
 *     LOG(ERROR) << "something wrong";
 *
 * 原理：LOG(X) 构造一个临时的 LogLine 对象，你往它身上用 << 拼字符串，
 * 这一整条语句结束时临时对象析构，在析构函数里把攒好的内容一次性打印出来
 * （自动换行）。这就是 glog 那种“流式日志”的最小实现。
 */
#pragma once

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace demo {

class LogLine {
 public:
  LogLine(const char* level, bool fatal) : fatal_(fatal) {
    stream_ << "[" << level << "] ";
  }

  // 析构时统一输出：这样一条 LOG(...) << a << b << c 只打印成一行。
  ~LogLine() {
    stream_ << "\n";
    std::cerr << stream_.str();
    std::cerr.flush();
    if (fatal_) {
      std::abort();  // 模仿 glog 的 LOG(FATAL)：打印后直接终止进程
    }
  }

  // 让 LogLine 支持 << 任意类型：转发给内部的 stringstream。
  template <typename T>
  LogLine& operator<<(const T& value) {
    stream_ << value;
    return *this;
  }

 private:
  std::ostringstream stream_;
  bool fatal_;
};

}  // namespace demo

// 四个日志级别的宏。用法和 glog 完全一致：LOG(INFO) << ...
#define LOG(level) LOG_##level
#define LOG_INFO ::demo::LogLine("INFO", false)
#define LOG_WARNING ::demo::LogLine("WARN", false)
#define LOG_ERROR ::demo::LogLine("ERROR", false)
#define LOG_FATAL ::demo::LogLine("FATAL", true)
