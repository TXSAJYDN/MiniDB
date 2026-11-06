#include "base/Log.h"
#include <iostream>

namespace minidb {

LogLevel Log::min_level_ = LogLevel::Info;

void Log::Write(LogLevel level, const std::string& msg) {
    if (level < min_level_) {
        return;
    }

    const char* level_str[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
    std::cerr << "[" << level_str[static_cast<int>(level)] << "] " << msg
              << '\n';
}

} // namespace minidb
