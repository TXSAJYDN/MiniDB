#include "CheckpointManifest.h"

#include "WalFormat.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace fs = std::filesystem;

namespace minidb {
namespace {

std::uint16_t load_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t load_u32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

std::uint64_t load_u64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

void store_u16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void store_u32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

void store_u64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string manifest_parent(const std::string& path) {
    fs::path parent = fs::path(path).parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    return parent.string();
}

} // namespace

bool CheckpointManifestCodec::validate(const CheckpointManifest& manifest,
                                       std::string* error) {
    if (manifest.retained_segment_starts.size() >
        CheckpointManifest::MAX_RETAINED_SEGMENTS) {
        set_error(error, "manifest retained segment count exceeds limit");
        return false;
    }
    std::uint64_t previous = 0;
    for (std::uint64_t start : manifest.retained_segment_starts) {
        if (start == 0 || start <= previous) {
            set_error(error,
                      "manifest retained segment starts are not increasing");
            return false;
        }
        previous = start;
    }
    return true;
}

std::size_t
CheckpointManifestCodec::encoded_size(const CheckpointManifest& manifest) {
    return CheckpointManifest::HEADER_SIZE +
           manifest.retained_segment_starts.size() * sizeof(std::uint64_t) +
           CheckpointManifest::CHECKSUM_SIZE;
}

std::vector<std::uint8_t>
CheckpointManifestCodec::encode(const CheckpointManifest& manifest,
                                std::string* error) {
    if (!validate(manifest, error)) {
        return {};
    }
    const std::size_t size = encoded_size(manifest);
    std::vector<std::uint8_t> result(size);
    std::memcpy(result.data(), CheckpointManifest::MAGIC, 4);
    store_u16(result.data() + 4, CheckpointManifest::VERSION);
    store_u16(result.data() + 6, CheckpointManifest::FLAGS);
    store_u64(result.data() + 8, manifest.checkpoint_lsn);
    store_u32(result.data() + 16, static_cast<std::uint32_t>(
                                      manifest.retained_segment_starts.size()));
    store_u32(result.data() + 20, 0);
    std::size_t offset = CheckpointManifest::HEADER_SIZE;
    for (std::uint64_t start : manifest.retained_segment_starts) {
        store_u64(result.data() + offset, start);
        offset += sizeof(std::uint64_t);
    }
    const std::size_t crc_offset = size - CheckpointManifest::CHECKSUM_SIZE;
    store_u32(result.data() + crc_offset,
              WalFormat::crc32c(result.data(), crc_offset));
    return result;
}

CheckpointManifest::DecodeStatus
CheckpointManifestCodec::decode(const std::uint8_t* data, std::size_t len,
                                CheckpointManifest* manifest,
                                std::size_t* consumed, std::string* error) {
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (data == nullptr || manifest == nullptr) {
        set_error(error, "manifest decoder received null input");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    if (len < 4) {
        return CheckpointManifest::DecodeStatus::kNeedMore;
    }
    if (std::memcmp(data, CheckpointManifest::MAGIC, 4) != 0) {
        set_error(error, "manifest magic mismatch");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    if (len < CheckpointManifest::HEADER_SIZE) {
        return CheckpointManifest::DecodeStatus::kNeedMore;
    }
    if (load_u16(data + 4) != CheckpointManifest::VERSION) {
        set_error(error, "manifest version mismatch");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    if (load_u16(data + 6) != CheckpointManifest::FLAGS) {
        set_error(error, "manifest flags are unsupported");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    const std::uint32_t retained_count = load_u32(data + 16);
    if (retained_count > CheckpointManifest::MAX_RETAINED_SEGMENTS) {
        set_error(error, "manifest retained segment count exceeds limit");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    if (load_u32(data + 20) != 0) {
        set_error(error, "manifest reserved field is non-zero");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    const std::uint64_t total_u64 =
        static_cast<std::uint64_t>(CheckpointManifest::HEADER_SIZE) +
        static_cast<std::uint64_t>(retained_count) * 8 +
        CheckpointManifest::CHECKSUM_SIZE;
    if (total_u64 > CheckpointManifest::MAX_FILE_SIZE ||
        total_u64 > std::numeric_limits<std::size_t>::max()) {
        set_error(error, "manifest size overflows");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    const std::size_t total = static_cast<std::size_t>(total_u64);
    if (len < total) {
        return CheckpointManifest::DecodeStatus::kNeedMore;
    }
    const std::size_t crc_offset = total - CheckpointManifest::CHECKSUM_SIZE;
    if (load_u32(data + crc_offset) != WalFormat::crc32c(data, crc_offset)) {
        set_error(error, "manifest checksum mismatch");
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }

    CheckpointManifest decoded;
    decoded.checkpoint_lsn = load_u64(data + 8);
    decoded.retained_segment_starts.reserve(retained_count);
    std::size_t offset = CheckpointManifest::HEADER_SIZE;
    for (std::uint32_t i = 0; i < retained_count; ++i) {
        decoded.retained_segment_starts.push_back(load_u64(data + offset));
        offset += 8;
    }
    if (!validate(decoded, error)) {
        return CheckpointManifest::DecodeStatus::kCorrupt;
    }
    *manifest = std::move(decoded);
    if (consumed != nullptr) {
        *consumed = total;
    }
    return CheckpointManifest::DecodeStatus::kOk;
}

ManifestLoadStatus load_manifest(const std::string& path,
                                 CheckpointManifest* manifest,
                                 std::string* error) {
    if (manifest == nullptr) {
        set_error(error, "manifest output is null");
        return ManifestLoadStatus::kCorrupt;
    }
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        status.type() == fs::file_type::not_found) {
        return ManifestLoadStatus::kMissing;
    }
    if (ec || !fs::is_regular_file(status)) {
        set_error(error, "manifest is not a regular readable file");
        return ManifestLoadStatus::kCorrupt;
    }
    const std::uintmax_t size_u = fs::file_size(path, ec);
    if (ec || size_u > CheckpointManifest::MAX_FILE_SIZE ||
        size_u < CheckpointManifest::HEADER_SIZE +
                     CheckpointManifest::CHECKSUM_SIZE) {
        set_error(error, "manifest size is outside the accepted range");
        return ManifestLoadStatus::kCorrupt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        set_error(error, "failed to open manifest");
        return ManifestLoadStatus::kCorrupt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size_u));
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        set_error(error, "failed to read the complete manifest");
        return ManifestLoadStatus::kCorrupt;
    }
    std::size_t consumed = 0;
    const auto decode_status = CheckpointManifestCodec::decode(
        bytes.data(), bytes.size(), manifest, &consumed, error);
    if (decode_status != CheckpointManifest::DecodeStatus::kOk ||
        consumed != bytes.size()) {
        if (error != nullptr && error->empty()) {
            *error = "manifest has trailing or incomplete bytes";
        }
        return ManifestLoadStatus::kCorrupt;
    }
    return ManifestLoadStatus::kOk;
}

ManifestWriteResult write_manifest_atomic(const std::string& path,
                                          const CheckpointManifest& manifest,
                                          WalFileOperations* operations,
                                          std::string* error) {
    if (operations == nullptr) {
        set_error(error, "manifest operations are null");
        return ManifestWriteResult::kFailed;
    }
    std::string encode_error;
    const std::vector<std::uint8_t> bytes =
        CheckpointManifestCodec::encode(manifest, &encode_error);
    if (bytes.empty()) {
        set_error(error, encode_error);
        return ManifestWriteResult::kFailed;
    }

    static std::atomic<std::uint64_t> sequence{0};
    const std::string temp_path =
        path + ".tmp." + std::to_string(::getpid()) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    int fd = operations->open_exclusive(temp_path, 0600);
    if (fd < 0) {
        set_error(error, "failed to create manifest temporary file");
        return ManifestWriteResult::kFailed;
    }
    if (!write_all(operations, fd, bytes.data(), bytes.size(), error)) {
        const int saved_errno = errno;
        operations->close_fd(fd);
        operations->unlink_file(temp_path);
        errno = saved_errno;
        return ManifestWriteResult::kFailed;
    }
    if (operations->fsync_fd(fd) < 0) {
        const int saved_errno = errno;
        operations->close_fd(fd);
        operations->unlink_file(temp_path);
        errno = saved_errno;
        set_error(error, "manifest fsync failed");
        return ManifestWriteResult::kFailed;
    }
    if (operations->close_fd(fd) < 0) {
        const int saved_errno = errno;
        operations->unlink_file(temp_path);
        errno = saved_errno;
        set_error(error, "manifest close failed");
        return ManifestWriteResult::kFailed;
    }
    if (operations->rename_file(temp_path, path) < 0) {
        const int saved_errno = errno;
        operations->unlink_file(temp_path);
        errno = saved_errno;
        set_error(error, "manifest rename failed");
        return ManifestWriteResult::kFailed;
    }
    if (!fsync_directory(manifest_parent(path), operations, error)) {
        return ManifestWriteResult::kDurabilityUnconfirmed;
    }
    return ManifestWriteResult::kOk;
}

} // namespace minidb
