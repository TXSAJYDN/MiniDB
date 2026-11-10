#pragma once

#include "WalFileOperations.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace minidb {

// ADR-002 manifest wire format, all integers big-endian:
//
//   "MMAN" | version:u16 | flags:u16 | checkpoint_lsn:u64
//          | retained_count:u32 | reserved:u32 | start_lsn:u64 * count
//          | crc32c:u32
//
// The manifest names a durable RDB checkpoint and the WAL segments that must
// exist to bridge from that checkpoint to the newest committed LSN.  A missing
// listed segment is corruption, never a reason to guess.
struct CheckpointManifest {
    static constexpr char MAGIC[5] = "MMAN";
    static constexpr std::uint16_t VERSION = 1;
    static constexpr std::uint16_t FLAGS = 0;
    static constexpr std::size_t HEADER_SIZE = 24;
    static constexpr std::size_t CHECKSUM_SIZE = 4;
    static constexpr std::uint32_t MAX_RETAINED_SEGMENTS = 128;
    static constexpr std::size_t MAX_FILE_SIZE =
        HEADER_SIZE + std::size_t{MAX_RETAINED_SEGMENTS} * 8 + CHECKSUM_SIZE;

    std::uint64_t checkpoint_lsn = 0;
    std::vector<std::uint64_t> retained_segment_starts;

    enum class DecodeStatus : std::uint8_t { kOk, kNeedMore, kCorrupt };
};

class CheckpointManifestCodec {
  public:
    static bool validate(const CheckpointManifest& manifest,
                         std::string* error = nullptr);
    static std::size_t encoded_size(const CheckpointManifest& manifest);
    static std::vector<std::uint8_t> encode(const CheckpointManifest& manifest,
                                            std::string* error = nullptr);
    static CheckpointManifest::DecodeStatus
    decode(const std::uint8_t* data, std::size_t len,
           CheckpointManifest* manifest, std::size_t* consumed = nullptr,
           std::string* error = nullptr);
};

enum class ManifestLoadStatus : std::uint8_t { kOk, kMissing, kCorrupt };
enum class ManifestWriteResult : std::uint8_t {
    kOk,
    kFailed,
    kDurabilityUnconfirmed,
};

ManifestLoadStatus load_manifest(const std::string& path,
                                 CheckpointManifest* manifest,
                                 std::string* error = nullptr);
ManifestWriteResult write_manifest_atomic(const std::string& path,
                                          const CheckpointManifest& manifest,
                                          WalFileOperations* operations,
                                          std::string* error = nullptr);

} // namespace minidb
