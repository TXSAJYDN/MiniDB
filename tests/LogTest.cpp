#include "base/Log.h"
#include <gtest/gtest.h>

namespace minidb {

TEST(LogTest, SetAndGetLevel) {
    Log::SetLevel(LogLevel::Warning);
    EXPECT_EQ(Log::GetLevel(), LogLevel::Warning);

    Log::SetLevel(LogLevel::Info);
    EXPECT_EQ(Log::GetLevel(), LogLevel::Info);
}

TEST(LogTest, BasicLogging) {
    Log::SetLevel(LogLevel::Debug);

    Log::Debug("Debug message");
    Log::Info("Info message");
    Log::Warning("Warning message");
    Log::Error("Error message");
}

TEST(LogTest, LevelFiltering) {
    Log::SetLevel(LogLevel::Error);

    Log::Debug("Should be filtered");
    Log::Info("Should be filtered");
    Log::Warning("Should be filtered");
    Log::Error("Should be logged");
}

} // namespace minidb
