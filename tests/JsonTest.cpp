#include <gtest/gtest.h>

#include "../src/protocol/Json.h"

#include <cstddef>
#include <string>

using namespace minidb;

TEST(JsonEscapeTest, EscapesQuotesBackslashesAndShortControls) {
    EXPECT_EQ(json_escape("plain text"), "plain text");
    EXPECT_EQ(json_escape("\""), "\\\"");
    EXPECT_EQ(json_escape("\\"), "\\\\");
    EXPECT_EQ(json_escape(std::string("\b\f\n\r\t", 5)), "\\b\\f\\n\\r\\t");
}

TEST(JsonEscapeTest, EscapesEveryOtherControlByteAsUnicode) {
    const std::string expected[] = {
        "\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005",
        "\\u0006", "\\u0007", "\\b",     "\\t",     "\\n",     "\\u000B",
        "\\f",     "\\r",     "\\u000E", "\\u000F", "\\u0010", "\\u0011",
        "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017",
        "\\u0018", "\\u0019", "\\u001A", "\\u001B", "\\u001C", "\\u001D",
        "\\u001E", "\\u001F",
    };
    for (std::size_t byte = 0; byte < 0x20; ++byte) {
        EXPECT_EQ(json_escape(std::string(1, static_cast<char>(byte))),
                  expected[byte])
            << "byte 0x" << std::hex << byte;
    }
}

TEST(JsonEscapeTest, PreservesNonControlBytesIncludingUtf8) {
    const std::string utf8 = std::string("\xE4\xB8\xAD\xF0\x9F\x8E\xAD", 7);
    EXPECT_EQ(json_escape(utf8), utf8);

    const std::string bytes = std::string("\x7F\x80\xFF", 3);
    EXPECT_EQ(json_escape(bytes), bytes);
}
