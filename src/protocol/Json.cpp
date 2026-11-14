#include "Json.h"

namespace minidb {
namespace {

char hex_digit(unsigned int value) {
    return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10));
}

} // namespace

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (byte < 0x20) {
                escaped += "\\u00";
                escaped.push_back(hex_digit(byte >> 4));
                escaped.push_back(hex_digit(byte & 0x0f));
            } else {
                escaped.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    return escaped;
}

} // namespace minidb
