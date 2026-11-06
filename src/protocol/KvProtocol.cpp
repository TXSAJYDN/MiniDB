#include "KvProtocol.h"

#include <arpa/inet.h>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace minidb {

std::vector<uint8_t>
KvProtocol::encode_command(CommandType type,
                           const std::vector<std::string>& args) {
    if (args.size() > kMaxArgumentCount) {
        throw std::invalid_argument("too many KV arguments");
    }

    uint64_t total_len = 2;
    for (const auto& arg : args) {
        if (arg.size() > kMaxArgumentSize) {
            throw std::invalid_argument("KV argument exceeds size limit");
        }
        total_len += 4 + static_cast<uint64_t>(arg.size());
    }
    if (total_len > kMaxFrameSize) {
        throw std::invalid_argument("KV frame exceeds size limit");
    }

    std::vector<uint8_t> result;
    result.reserve(static_cast<size_t>(4 + total_len));

    const uint32_t len_net = htonl(static_cast<uint32_t>(total_len));
    const uint8_t* len_bytes = reinterpret_cast<const uint8_t*>(&len_net);
    result.insert(result.end(), len_bytes, len_bytes + 4);
    result.push_back(static_cast<uint8_t>(type));
    result.push_back(static_cast<uint8_t>(args.size()));

    for (const auto& arg : args) {
        const uint32_t arg_len_net = htonl(static_cast<uint32_t>(arg.size()));
        const uint8_t* arg_len_bytes =
            reinterpret_cast<const uint8_t*>(&arg_len_net);
        result.insert(result.end(), arg_len_bytes, arg_len_bytes + 4);
        result.insert(result.end(), arg.begin(), arg.end());
    }
    return result;
}

int KvProtocol::parse_command(const uint8_t* data, size_t len, Command* cmd) {
    if (data == nullptr || cmd == nullptr) {
        return -1;
    }
    if (len < 4) {
        return 0;
    }

    uint32_t msg_len_net = 0;
    std::memcpy(&msg_len_net, data, 4);
    const uint32_t msg_len = ntohl(msg_len_net);
    if (msg_len < 2 || msg_len > kMaxFrameSize) {
        return -1;
    }

    const size_t frame_end = 4u + static_cast<size_t>(msg_len);
    if (len < frame_end) {
        return 0;
    }

    size_t offset = 4;
    const uint8_t cmd_type = data[offset++];
    const uint8_t arg_count = data[offset++];

    Command parsed;
    parsed.type = cmd_type;
    parsed.args.clear();
    parsed.args.reserve(arg_count);

    for (uint8_t i = 0; i < arg_count; ++i) {
        if (frame_end - offset < 4) {
            return -1;
        }
        uint32_t arg_len_net = 0;
        std::memcpy(&arg_len_net, data + offset, 4);
        const uint32_t arg_len = ntohl(arg_len_net);
        offset += 4;

        if (arg_len > kMaxArgumentSize ||
            static_cast<size_t>(arg_len) > frame_end - offset) {
            return -1;
        }
        parsed.args.emplace_back(reinterpret_cast<const char*>(data + offset),
                                 arg_len);
        offset += arg_len;
    }

    if (offset != frame_end) {
        return -1;
    }

    *cmd = std::move(parsed);
    return static_cast<int>(frame_end);
}

std::string KvProtocol::command_type_to_string(uint8_t type) {
    switch (type) {
    case kPing:
        return "PING";
    case kSet:
        return "SET";
    case kGet:
        return "GET";
    case kDel:
        return "DEL";
    case kExpire:
        return "EXPIRE";
    case kTtl:
        return "TTL";
    case kSave:
        return "SAVE";
    case kOk:
        return "OK";
    case kError:
        return "ERROR";
    case kValue:
        return "VALUE";
    case kNotFound:
        return "NOTFOUND";
    case kInteger:
        return "INTEGER";
    default:
        return "UNKNOWN";
    }
}

} // namespace minidb
