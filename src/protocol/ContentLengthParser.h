#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace minidb {

class ContentLengthParser {
  public:
    enum class Status : std::uint8_t { kOk, kInvalid, kTooLarge };

    // Parses Content-Length as a complete non-negative decimal integer.
    // Leading zeroes are allowed ("05" == 5). Signs, whitespace, an empty
    // value and non-digits are rejected; values above the 1 MiB body limit
    // are reported as kTooLarge so HTTP can return 413 rather than 400.
    static Status parse_checked(const std::string& value, size_t* parsed) {
        if (parsed == nullptr || value.empty()) {
            return Status::kInvalid;
        }

        uint64_t result = 0;
        for (unsigned char c : value) {
            if (c < '0' || c > '9') {
                return Status::kInvalid;
            }
            const uint64_t digit = static_cast<uint64_t>(c - '0');
            if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10u) {
                return Status::kTooLarge;
            }
            result = result * 10u + digit;
            if (result > kMaxContentLength) {
                return Status::kTooLarge;
            }
        }

        *parsed = static_cast<size_t>(result);
        return Status::kOk;
    }

    static std::optional<size_t> parse(const std::string& value) {
        size_t parsed = 0;
        return parse_checked(value, &parsed) == Status::kOk
                   ? std::optional<size_t>(parsed)
                   : std::nullopt;
    }

    static constexpr uint64_t kMaxContentLength = uint64_t{1} * 1024 * 1024;
};

} // namespace minidb
