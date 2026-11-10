#include "../src/storage/WalFormat.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message) {
    __builtin_trap();
    (void)message;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    // Keep the fuzzer allocation ceiling close to the largest legal record.
    // Larger inputs are rejected before the codec can materialize a payload.
    if (size > minidb::WalFormat::MAX_RECORD_SIZE + 64u) {
        return 0;
    }

    minidb::WalRecord decoded;
    std::size_t consumed = 0;
    const minidb::WalFormat::DecodeStatus status =
        minidb::WalFormat::decode(data, size, &decoded, &consumed);
    if (status == minidb::WalFormat::DecodeStatus::kOk) {
        if (consumed == 0 || consumed > size) {
            fail("decoder returned an invalid consumed size");
        }

        std::string error;
        const std::vector<std::uint8_t> encoded =
            minidb::WalFormat::encode(decoded, &error);
        if (encoded.empty() || encoded.size() != consumed) {
            fail("valid decoded record did not re-encode");
        }

        minidb::WalRecord roundtrip;
        std::size_t roundtrip_consumed = 0;
        const minidb::WalFormat::DecodeStatus roundtrip_status =
            minidb::WalFormat::decode(encoded.data(), encoded.size(),
                                      &roundtrip, &roundtrip_consumed);
        if (roundtrip_status != minidb::WalFormat::DecodeStatus::kOk ||
            roundtrip_consumed != encoded.size() ||
            roundtrip.lsn != decoded.lsn || roundtrip.op != decoded.op ||
            roundtrip.key != decoded.key || roundtrip.value != decoded.value ||
            roundtrip.expiry_ms != decoded.expiry_ms) {
            fail("WAL encode/decode round-trip changed the record");
        }
    }

    // Exercise the public encoder with a bounded synthetic record as well.
    // This gives the mutation engine a second path even when raw input does
    // not contain a valid CRC.
    if (size >= 3) {
        minidb::WalRecord synthetic;
        synthetic.lsn = 1;
        synthetic.op = static_cast<minidb::WalOp>(1 + (data[0] % 3));
        synthetic.key.assign(reinterpret_cast<const char*>(data + 1),
                             std::min<std::size_t>(data[1] % 64u, size - 1));
        if (synthetic.op == minidb::WalOp::kSet) {
            const std::size_t value_len =
                std::min<std::size_t>(data[2] % 256u, size - 2);
            synthetic.value.assign(reinterpret_cast<const char*>(data + 2),
                                   value_len);
        } else if (synthetic.op == minidb::WalOp::kExpire) {
            synthetic.expiry_ms = 1 + static_cast<std::int64_t>(data[2]);
        }
        if (minidb::WalFormat::validate(synthetic)) {
            const std::vector<std::uint8_t> encoded =
                minidb::WalFormat::encode(synthetic);
            if (encoded.empty()) {
                fail("valid synthetic record did not encode");
            }
        }
    }
    return 0;
}
