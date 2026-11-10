#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace minidb {

// Binary-safe KV wire format:
//   frame_len:u32 BE | command:u8 | argc:u8 |
//   (arg_len:u32 BE | arg_bytes) * argc
// frame_len covers command through the final argument, excluding itself.
class KvProtocol {
  public:
    enum CommandType : uint8_t {
        kPing = 1,
        kSet = 2,
        kGet = 3,
        kDel = 4,
        kExpire = 5,
        kTtl = 6,
        kSave = 7,
        kStats = 8,
        kError = 100,
        kOk = 101,
        kValue = 102,
        kNotFound = 103,
        kInteger = 104,
    };

    struct Command {
        // Keep the raw wire byte so unknown commands can be rejected by the
        // session instead of creating an out-of-range enum value.
        uint8_t type = 0;
        std::vector<std::string> args;
    };

    static constexpr uint32_t kMaxArgumentSize = 1024u * 1024u;
    // A legal SET can carry a 1 KiB key and a 1 MiB value.  The frame limit
    // must therefore include protocol framing and both arguments, not just the
    // value payload.  The reserve also leaves room for response framing.
    static constexpr uint32_t kMaxFrameSize = kMaxArgumentSize + (64u * 1024u);
    static constexpr size_t kMaxKeySize = 1024;
    static constexpr size_t kMaxArgumentCount = 255;

    // Throws std::invalid_argument for a command that cannot be represented
    // without narrowing or exceeding the wire limits.
    static std::vector<uint8_t>
    encode_command(CommandType type, const std::vector<std::string>& args);

    // Returns the consumed frame length, 0 when more bytes are needed, or -1
    // for malformed input.  The declared frame must be consumed exactly.
    static int parse_command(const uint8_t* data, size_t len, Command* cmd);

    static std::string command_type_to_string(uint8_t type);
};

} // namespace minidb
