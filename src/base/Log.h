#ifndef MINIDB_BASE_LOG_H
#define MINIDB_BASE_LOG_H

#include <cstdint>
#include <string>

namespace minidb {

enum class LogLevel : std::uint8_t { Debug, Info, Warning, Error };

class Log {
  public:
    static void SetLevel(LogLevel level) {
        min_level_ = level;
    }
    static LogLevel GetLevel() {
        return min_level_;
    }

    static void Debug(const std::string& msg) {
        Write(LogLevel::Debug, msg);
    }
    static void Info(const std::string& msg) {
        Write(LogLevel::Info, msg);
    }
    static void Warning(const std::string& msg) {
        Write(LogLevel::Warning, msg);
    }
    static void Error(const std::string& msg) {
        Write(LogLevel::Error, msg);
    }

    static void Write(LogLevel level, const std::string& msg);

  private:
    static LogLevel min_level_;
};

} // namespace minidb

#endif // MINIDB_BASE_LOG_H
