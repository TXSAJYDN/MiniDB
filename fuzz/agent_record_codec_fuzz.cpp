#include "../src/storage/AgentRecordCodec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    std::string error;

    minidb::AgentRecordCodec::KeyInfo key_info;
    minidb::AgentRecordCodec::Record record;
    minidb::AgentRecordCodec::Response response;
    minidb::AgentRecordCodec::SessionPayload session;
    minidb::AgentRecordCodec::ToolPayload tool;
    minidb::AgentRecordCodec::IdempotencyPayload idempotency;

    minidb::AgentRecordCodec::decode_marker(input, &error);
    const bool valid_key =
        minidb::AgentRecordCodec::decode_key(input, &key_info, &error);
    const bool valid_record =
        minidb::AgentRecordCodec::decode_record(input, &record, &error);
    minidb::AgentRecordCodec::decode_response(input, &response, &error);
    minidb::AgentRecordCodec::decode_session_payload(input, &session, &error);
    minidb::AgentRecordCodec::decode_tool_payload(input, &tool, &error);
    minidb::AgentRecordCodec::decode_idempotency_payload(input, &idempotency,
                                                         &error);

    if (valid_key && valid_record) {
        std::string key(input);
        minidb::AgentRecordCodec::decode_typed_record(
            key, input, nullptr, nullptr, nullptr, nullptr, &error);
    }
    return 0;
}
