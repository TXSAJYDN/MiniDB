#include "../src/storage/CheckpointManifest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message) {
    __builtin_trap();
    (void)message;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    if (size > minidb::CheckpointManifest::MAX_FILE_SIZE + 64u) {
        return 0;
    }

    minidb::CheckpointManifest decoded;
    std::size_t consumed = 0;
    const minidb::CheckpointManifest::DecodeStatus status =
        minidb::CheckpointManifestCodec::decode(data, size, &decoded,
                                                &consumed);
    if (status == minidb::CheckpointManifest::DecodeStatus::kOk) {
        if (consumed == 0 || consumed > size) {
            fail("manifest decoder returned an invalid consumed size");
        }
        if (!minidb::CheckpointManifestCodec::validate(decoded)) {
            fail("decoded manifest failed validation");
        }

        std::string error;
        const std::vector<std::uint8_t> encoded =
            minidb::CheckpointManifestCodec::encode(decoded, &error);
        if (encoded.empty() || encoded.size() != consumed) {
            fail("valid decoded manifest did not re-encode");
        }

        minidb::CheckpointManifest roundtrip;
        std::size_t roundtrip_consumed = 0;
        const minidb::CheckpointManifest::DecodeStatus roundtrip_status =
            minidb::CheckpointManifestCodec::decode(encoded.data(),
                                                    encoded.size(), &roundtrip,
                                                    &roundtrip_consumed);
        if (roundtrip_status != minidb::CheckpointManifest::DecodeStatus::kOk ||
            roundtrip_consumed != encoded.size() ||
            roundtrip.checkpoint_lsn != decoded.checkpoint_lsn ||
            roundtrip.retained_segment_starts !=
                decoded.retained_segment_starts) {
            fail("manifest encode/decode round-trip changed the manifest");
        }
    }

    // Build a bounded manifest from input bytes.  This exercises ordering,
    // zero-LSN, and retained-count validation independently of the wire CRC.
    if (size >= 2) {
        minidb::CheckpointManifest synthetic;
        synthetic.checkpoint_lsn =
            (static_cast<std::uint64_t>(data[0]) << 8) | data[1];
        const std::size_t count = std::min<std::size_t>(
            size - 2, minidb::CheckpointManifest::MAX_RETAINED_SEGMENTS);
        synthetic.retained_segment_starts.reserve(count);
        std::uint64_t next = synthetic.checkpoint_lsn + 1;
        for (std::size_t i = 0; i < count; ++i) {
            next += data[2 + i] % 7u;
            synthetic.retained_segment_starts.push_back(next);
        }
        if (minidb::CheckpointManifestCodec::validate(synthetic)) {
            const std::vector<std::uint8_t> encoded =
                minidb::CheckpointManifestCodec::encode(synthetic);
            if (encoded.empty()) {
                fail("valid synthetic manifest did not encode");
            }
        }
    }
    return 0;
}
