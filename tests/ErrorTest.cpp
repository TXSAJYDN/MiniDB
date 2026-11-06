#include "base/Error.h"
#include <gtest/gtest.h>

namespace minidb {

TEST(ErrorTest, DefaultConstructor) {
    Error err;
    EXPECT_TRUE(err.ok());
    EXPECT_EQ(err.code(), ErrorCode::Success);
    EXPECT_TRUE(err.message().empty());
}

TEST(ErrorTest, ErrorWithMessage) {
    Error err(ErrorCode::IOError, "File not found");
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code(), ErrorCode::IOError);
    EXPECT_EQ(err.message(), "File not found");
}

TEST(ErrorTest, StaticFactories) {
    auto ok = Error::OK();
    EXPECT_TRUE(ok.ok());

    auto invalid = Error::InvalidArgument("Bad param");
    EXPECT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(invalid.message(), "Bad param");

    auto io = Error::IOError("Disk full");
    EXPECT_EQ(io.code(), ErrorCode::IOError);
}

} // namespace minidb
