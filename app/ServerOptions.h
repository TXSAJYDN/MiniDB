#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

namespace minidb {

struct ServerOptions {
    uint16_t port = 0;
    std::string rdb_path;
    bool help_requested = false;
};

// Parses the shared server command line.  Only '-p <port>', '-r <path>' and
// the standalone '-h' are accepted.  Unknown options, missing values, extra
// positional arguments, duplicate options and non-canonical decimal ports
// (including '+1', whitespace and trailing characters) are rejected.
inline bool parse_server_options(int argc, char* const argv[],
                                 uint16_t default_port, ServerOptions* options,
                                 std::string* error) {
    if (options == nullptr || error == nullptr) {
        return false;
    }

    ServerOptions parsed;
    parsed.port = default_port;
    bool have_p = false;
    bool have_r = false;
    bool have_h = false;

    auto fail = [&](const std::string& message) {
        *error = message;
        return false;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] == nullptr ? std::string() : argv[i];
        if (arg == "-h") {
            if (have_h || have_p || have_r || argc != 2) {
                return fail("'-h' must be used alone");
            }
            have_h = true;
            parsed.help_requested = true;
            continue;
        }
        if (arg == "-p" || arg == "-r") {
            if (i + 1 >= argc) {
                return fail("missing value for " + arg);
            }
            const std::string value =
                argv[++i] == nullptr ? std::string() : argv[i];
            if (value.empty()) {
                return fail("empty value for " + arg);
            }
            if (arg == "-p") {
                if (have_p) {
                    return fail("duplicate option '-p'");
                }
                have_p = true;
                uint32_t port = 0;
                const char* begin = value.data();
                const char* end = begin + value.size();
                const auto result = std::from_chars(begin, end, port);
                if (result.ec != std::errc{} || result.ptr != end || port < 1 ||
                    port > 65535) {
                    return fail("invalid port '" + value +
                                "' (expected decimal 1..65535)");
                }
                parsed.port = static_cast<uint16_t>(port);
            } else {
                if (have_r) {
                    return fail("duplicate option '-r'");
                }
                have_r = true;
                parsed.rdb_path = value;
            }
            continue;
        }
        return fail("unknown or unexpected argument '" + arg + "'");
    }

    *options = std::move(parsed);
    return true;
}

} // namespace minidb
