#ifndef LOGGER_H
#define LOGGER_H

//=============================================================================
// logger.h — 轻量级日志工具
//
// 用法:
//   Logger::Info("ModuleName") << "Hello, " << 42 << std::endl;
//   Logger::Warn("ModuleName") << "Something odd: " << msg << std::endl;
//   Logger::Error("ModuleName") << "Failed: " << reason << std::endl;
//
//   Logger::SetLevel(Logger::DEBUG);      // 全局最低级别
//   Logger::SetModuleLevel("modbus", 2);  // 模块级覆盖
//
// 特点:
//   - 线程安全（互斥锁保护每行输出）
//   - 自动添加时间戳前缀
//   - 模块名对齐，便于 grep
//   - 头文件仅，无 .cxx 依赖
//=============================================================================

#include <iostream>
#include <sstream>
#include <mutex>
#include <map>
#include <string>
#include <chrono>
#include <cstdint>
#include <atomic>

class Logger {
public:
    enum Level : int {
        NONE  = 0,
        ERROR = 1,
        WARN  = 2,
        INFO  = 3,
        DEBUG = 4,
    };

    /// 全局日志级别（低于此级别的不输出）
    static void SetLevel(int lv) { globalLevel_.store(lv); }
    static int  GetLevel()       { return globalLevel_.load(); }

    /// 某模块的日志级别（0=不输出，>0 覆盖全局级别）
    static void SetModuleLevel(const std::string& module, int lv) {
        std::lock_guard<std::mutex> lock(moduleMtx_);
        moduleLevels_[module] = lv;
    }

    /// 日志流式对象，析构时输出
    class LogStream {
    public:
        LogStream(int level, const char* tag) : level_(level), tag_(tag) {
            // 时间戳前缀： "12:34:56.789 "
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() % 1000;
            struct tm t;
#ifdef _WIN32
            localtime_s(&t, &tt);
#else
            localtime_r(&tt, &t);
#endif
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03llu ",
                          t.tm_hour, t.tm_min, t.tm_sec,
                          (unsigned long long)ms);
            ts_ = buf;
        }
        ~LogStream() {
            std::lock_guard<std::mutex> lock(mtx_);
            std::cout << ts_ << "[" << levelChar(level_) << "][" << tag_ << "] "
                      << ss_.str() << std::flush;
        }
        template<typename T>
        LogStream& operator<<(const T& v) { ss_ << v; return *this; }
        LogStream& operator<<(std::ostream& (*f)(std::ostream&)) {
            if (f == static_cast<std::ostream& (*)(std::ostream&)>(std::endl))
                ss_ << '\n';
            return *this;
        }
    private:
        static char levelChar(int lv) {
            switch (lv) {
                case ERROR: return 'E';
                case WARN:  return 'W';
                case INFO:  return 'I';
                case DEBUG: return 'D';
                default:    return '?';
            }
        }
        int level_;
        const char* tag_;
        std::string ts_;
        std::ostringstream ss_;
        static std::mutex mtx_;
    };

    /// 快捷输出方法（使用时先判断级别，避免构造 LogStream 开销）
    static LogStream Error(const char* tag) { return LogStream(ERROR, tag); }
    static LogStream Warn(const char* tag)  { return LogStream(WARN,  tag); }
    static LogStream Info(const char* tag)  { return LogStream(INFO,  tag); }
    static LogStream Debug(const char* tag) { return LogStream(DEBUG, tag); }

private:
    static std::atomic<int> globalLevel_;
    static std::map<std::string, int> moduleLevels_;
    static std::mutex moduleMtx_;
};

// 静态成员定义（头文件中需 inline 或外部定义，但此处为单头文件设计，
// 多个 TU 包含时需确保仅一个定义。对 .h 来说是合理的折衷。）
inline std::atomic<int> Logger::globalLevel_{Logger::INFO};
inline std::map<std::string, int> Logger::moduleLevels_;
inline std::mutex Logger::moduleMtx_;
inline std::mutex Logger::LogStream::mtx_;

#endif // LOGGER_H
