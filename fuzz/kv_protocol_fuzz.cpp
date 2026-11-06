#include "../src/protocol/KvProtocol.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    if (size > (static_cast<std::size_t>(minidb::KvProtocol::kMaxFrameSize) +
                64u * 1024u)) {
        return 0;
    }

    std::size_t offset = 0;
    int frames = 0;
    while (offset < size && frames < 16) {
        minidb::KvProtocol::Command command;
        const int consumed = minidb::KvProtocol::parse_command(
            data + offset, size - offset, &command);
        if (consumed <= 0) {
            break;
        }
        ++frames;

        // Exercise the encoder with the widest parsed command.  Unknown
        // command bytes are intentionally representable and must round-trip
        // until session-level validation rejects them.
        try {
            const auto encoded = minidb::KvProtocol::encode_command(
                static_cast<minidb::KvProtocol::CommandType>(command.type),
                command.args);
            minidb::KvProtocol::Command reparsed;
            (void)minidb::KvProtocol::parse_command(encoded.data(),
                                                    encoded.size(), &reparsed);
        } catch (const std::invalid_argument&) {
            // Inputs near the frame boundary may be parseable but not
            // encodable once command/argument framing is added.
        }
        offset += static_cast<std::size_t>(consumed);
    }
    return 0;
}
