#include "../src/storage/AgentRecordCodec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    std::string error;

    minidb::AgentRecordCodec::decode_marker(input, &error);
    minidb::AgentRecordCodec::decode_key(input, nullptr, &error);
    minidb::AgentRecordCodec::decode_record(input, nullptr, &error);
    minidb::AgentRecordCodec::decode_response(input, nullptr, &error);
    minidb::AgentRecordCodec::decode_session_payload(input, nullptr, &error);
    minidb::AgentRecordCodec::decode_tool_payload(input, nullptr, &error);
    minidb::AgentRecordCodec::decode_idempotency_payload(input, nullptr,
                                                         &error);

    minidb::AgentRecordCodec::KeyInfo key_info;
    if (minidb::AgentRecordCodec::decode_key(input, &key_info, &error)) {
        std::string key(input);
        minidb::AgentRecordCodec::Record record;
        if (minidb::AgentRecordCodec::decode_record(input, &record, &error)) {
            minidb::AgentRecordCodec::decode_typed_record(
                key, input, nullptr, nullptr, nullptr, nullptr, &error);
        }
    }
    return 0;
}
