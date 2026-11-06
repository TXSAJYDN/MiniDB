#include <gtest/gtest.h>
#include "../src/protocol/ContentLengthParser.h"

#include <limits>
#include <string>

namespace minidb {

class ContentLengthParserTest : public ::testing::Test {};

TEST_F(ContentLengthParserTest, AcceptCanonicalAndLeadingZeroDecimal) {
    EXPECT_EQ(ContentLengthParser::parse("0"), std::optional<size_t>(0));
    EXPECT_EQ(ContentLengthParser::parse("1024"), std::optional<size_t>(1024));
    EXPECT_EQ(ContentLengthParser::parse("0001024"),
              std::optional<size_t>(1024));
    EXPECT_EQ(ContentLengthParser::parse("0000"), std::optional<size_t>(0));
    EXPECT_EQ(ContentLengthParser::parse("1048576"),
              std::optional<size_t>(1048576));
}

TEST_F(ContentLengthParserTest, RejectInvalidValues) {
    for (const std::string& value : {
             std::string(),
             std::string("+123"),
             std::string("-1"),
             std::string("123abc"),
             std::string("abc123"),
             std::string(" 123"),
             std::string("123 "),
             std::string("12 34"),
             std::string("0x10"),
         }) {
        EXPECT_FALSE(ContentLengthParser::parse(value).has_value())
            << "value=" << value;
    }
}

TEST_F(ContentLengthParserTest, RejectValuesAboveOneMiB) {
    EXPECT_FALSE(ContentLengthParser::parse("1048577").has_value());
    EXPECT_FALSE(ContentLengthParser::parse("10485760").has_value());
    EXPECT_FALSE(ContentLengthParser::parse("99999999999").has_value());

    size_t parsed = 0;
    EXPECT_EQ(ContentLengthParser::parse_checked("1048577", &parsed),
              ContentLengthParser::Status::kTooLarge);
    EXPECT_EQ(ContentLengthParser::parse_checked(
                  "999999999999999999999999999999999999", &parsed),
              ContentLengthParser::Status::kTooLarge);
}

TEST_F(ContentLengthParserTest, CheckedStatusDistinguishesInvalidFromTooLarge) {
    size_t parsed = 123;
    EXPECT_EQ(ContentLengthParser::parse_checked("", &parsed),
              ContentLengthParser::Status::kInvalid);
    EXPECT_EQ(ContentLengthParser::parse_checked("-1", &parsed),
              ContentLengthParser::Status::kInvalid);
    EXPECT_EQ(ContentLengthParser::parse_checked("x", &parsed),
              ContentLengthParser::Status::kInvalid);
    EXPECT_EQ(ContentLengthParser::parse_checked("1048577", &parsed),
              ContentLengthParser::Status::kTooLarge);
    EXPECT_EQ(ContentLengthParser::parse_checked("1048576", &parsed),
              ContentLengthParser::Status::kOk);
    EXPECT_EQ(parsed, size_t{1048576});
}

TEST_F(ContentLengthParserTest, RejectNullOutput) {
    EXPECT_EQ(ContentLengthParser::parse_checked("1", nullptr),
              ContentLengthParser::Status::kInvalid);
}

} // namespace minidb