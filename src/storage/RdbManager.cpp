#include "RdbManager.h"
#include "../base/Log.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

class PosixRdbFileOperations final : public RdbFileOperations {
  public:
    int open_exclusive(const std::string& path, mode_t mode) override {
        return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                      mode);
    }
    ssize_t write(int fd, const void* data, size_t len) override {
        return ::write(fd, data, len);
    }
    int fsync_fd(int fd) override {
        return ::fsync(fd);
    }
    int close_fd(int fd) override {
        return ::close(fd);
    }
    int rename_file(const std::string& from, const std::string& to) override {
        return ::rename(from.c_str(), to.c_str());
    }
    int unlink_file(const std::string& path) override {
        return ::unlink(path.c_str());
    }
    int open_parent_dir(const std::string& path) override {
        fs::path parent = fs::path(path).parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        return ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    int fsync_dir(int fd) override {
        return ::fsync(fd);
    }
};

bool write_all(RdbFileOperations* ops, int fd, const void* data, size_t len) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ops->write(fd, ptr, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        ptr += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    uint32_t network = RdbFormat::hton32(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    out.insert(out.end(), bytes, bytes + sizeof(network));
}

void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    uint64_t network = RdbFormat::hton64(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    out.insert(out.end(), bytes, bytes + sizeof(network));
}

bool append_length_prefixed(std::vector<uint8_t>& out,
                            const std::string& value) {
    auto encoded =
        RdbFormat::encode_varint(static_cast<uint32_t>(value.size()));
    if (value.size() + encoded.size() >
        RdbFormat::MAX_FILE_SIZE - RdbFormat::CHECKSUM_SIZE - out.size()) {
        return false;
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
    out.insert(out.end(), value.begin(), value.end());
    return true;
}

} // namespace

RdbManager::RdbManager(const std::string& rdb_path)
    : RdbManager(rdb_path, std::make_shared<PosixRdbFileOperations>()) {}

RdbManager::RdbManager(const std::string& rdb_path,
                       std::shared_ptr<RdbFileOperations> file_operations)
    : rdb_path_(rdb_path), file_operations_(std::move(file_operations)) {
    if (!file_operations_) {
        file_operations_ = std::make_shared<PosixRdbFileOperations>();
    }
}

RdbManager::~RdbManager() = default;

std::string RdbManager::make_temp_path() const {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t n = sequence.fetch_add(1, std::memory_order_relaxed);
    return rdb_path_ + ".tmp." + std::to_string(::getpid()) + "." +
           std::to_string(n);
}

bool RdbManager::exists() const {
    std::error_code ec;
    return fs::exists(rdb_path_, ec) && !ec;
}

bool RdbManager::remove() {
    std::error_code ec;
    const bool removed = fs::remove(rdb_path_, ec);
    return (!ec && removed) || (!ec && !fs::exists(rdb_path_));
}

RdbSaveResult RdbManager::last_save_result() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_save_result_;
}

bool RdbManager::save(const KvStore& store) {
    return save_checked(store) == RdbSaveResult::Ok;
}

RdbSaveResult RdbManager::save_checked(const KvStore& store) {
    // Lock order across RdbManager and KvStore is always
    // scanner_pause_mutex_ -> save_mutex_.  Checkpoint callers already hold a
    // ScannerPause before entering the overload below; acquiring the locks in
    // the opposite order here would permit a cross-thread deadlock.
    KvStore::ScannerPause scanner_pause = store.pause_scanner();
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    return save_checked_under_lock(store);
}

RdbSaveResult RdbManager::save_checked(const KvStore& store,
                                       KvStore::ScannerPause& scanner_pause) {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    // The lifetime of the caller pause is the checkpoint publication
    // boundary.  Referencing it here prevents accidental dead-code treatment
    // and documents that this overload must not acquire a second pause.
    (void)scanner_pause;
    return save_checked_under_lock(store);
}

bool RdbManager::capture(const KvStore& store, RdbSnapshot* snapshot,
                         std::string* error) {
    // Preserve the same scanner_pause_mutex_ -> save_mutex_ lock order as
    // save_checked().
    KvStore::ScannerPause scanner_pause = store.pause_scanner();
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    return capture_entries(store, snapshot, error);
}

bool RdbManager::capture(const KvStore& store,
                         KvStore::ScannerPause& scanner_pause,
                         RdbSnapshot* snapshot, std::string* error) {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    (void)scanner_pause;
    return capture_entries(store, snapshot, error);
}

bool RdbManager::capture_entries(const KvStore& store, RdbSnapshot* snapshot,
                                 std::string* error) {
    if (snapshot == nullptr) {
        if (error != nullptr) {
            *error = "RDB snapshot output is null";
        }
        return false;
    }
    RdbSnapshot captured;
    captured.entries = store.export_all();
    if (captured.entries.size() > RdbFormat::MAX_RECORDS) {
        if (error != nullptr) {
            *error = "RDB save rejected: record count exceeds limit";
        }
        Log::Error("RDB save rejected: record count exceeds limit");
        return false;
    }
    for (const auto& entry : captured.entries) {
        if (entry.key.empty() || entry.key.size() > RdbFormat::MAX_KEY_SIZE ||
            entry.value.size() > RdbFormat::MAX_VALUE_SIZE ||
            entry.expiry_ms < 0) {
            if (error != nullptr) {
                *error = "RDB save rejected: entry exceeds format limit";
            }
            Log::Error("RDB save rejected: entry exceeds format limit");
            return false;
        }
    }
    *snapshot = std::move(captured);
    return true;
}

bool RdbManager::serialize(const RdbSnapshot& snapshot,
                           std::vector<std::uint8_t>* image,
                           RdbSaveTimings* timings, std::string* error) {
    const std::uint64_t started_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const bool ok = serialize_entries(snapshot, image, error);
    if (timings != nullptr) {
        const std::uint64_t finished_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        timings->serialize_us =
            finished_us >= started_us ? finished_us - started_us : 0;
    }
    return ok;
}

bool RdbManager::serialize_entries(const RdbSnapshot& snapshot,
                                   std::vector<std::uint8_t>* image,
                                   std::string* error) {
    if (image == nullptr) {
        if (error != nullptr) {
            *error = "RDB image output is null";
        }
        return false;
    }
    std::vector<uint8_t> buffer;
    buffer.reserve(RdbFormat::HEADER_SIZE + RdbFormat::CHECKSUM_SIZE);
    buffer.insert(buffer.end(),
                  reinterpret_cast<const uint8_t*>(RdbFormat::MAGIC),
                  reinterpret_cast<const uint8_t*>(RdbFormat::MAGIC) + 4);
    append_u32(buffer, RdbFormat::VERSION);
    append_u32(buffer, static_cast<uint32_t>(snapshot.entries.size()));

    for (const auto& entry : snapshot.entries) {
        if (!append_length_prefixed(buffer, entry.key) ||
            !append_length_prefixed(buffer, entry.value)) {
            if (error != nullptr) {
                *error = "RDB save rejected: serialized image exceeds 64 MiB";
            }
            Log::Error("RDB save rejected: serialized image exceeds 64 MiB");
            return false;
        }
        if (buffer.size() >
            RdbFormat::MAX_FILE_SIZE - RdbFormat::CHECKSUM_SIZE - 8) {
            if (error != nullptr) {
                *error = "RDB save rejected: serialized image exceeds 64 MiB";
            }
            Log::Error("RDB save rejected: serialized image exceeds 64 MiB");
            return false;
        }
        append_u64(buffer, static_cast<uint64_t>(entry.expiry_ms));
    }
    const uint32_t checksum = RdbFormat::crc32c(buffer.data(), buffer.size());
    append_u32(buffer, checksum);
    *image = std::move(buffer);
    return true;
}

RdbSaveResult RdbManager::save_checked_under_lock(const KvStore& store) {
    RdbSnapshot snapshot;
    std::string capture_error;
    if (!capture_entries(store, &snapshot, &capture_error)) {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = RdbSaveResult::Failed;
        return RdbSaveResult::Failed;
    }
    std::vector<std::uint8_t> image;
    std::string serialize_error;
    if (!serialize_entries(snapshot, &image, &serialize_error)) {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = RdbSaveResult::Failed;
        return RdbSaveResult::Failed;
    }
    return write_durable_under_lock(image, nullptr, nullptr);
}

RdbSaveResult RdbManager::write_durable(const std::vector<std::uint8_t>& image,
                                        RdbSaveTimings* timings,
                                        std::string* error) {
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    return write_durable_under_lock(image, timings, error);
}

RdbSaveResult
RdbManager::write_durable_under_lock(const std::vector<std::uint8_t>& image,
                                     RdbSaveTimings* timings,
                                     std::string* error) {
    const auto now_us = []() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    const auto set_error = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
    };

    if (image.size() < RdbFormat::MIN_FILE_SIZE ||
        image.size() > RdbFormat::MAX_FILE_SIZE) {
        set_error("RDB image size is outside the accepted range");
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = RdbSaveResult::Failed;
        return RdbSaveResult::Failed;
    }

    RdbSaveResult result = RdbSaveResult::Failed;
    std::string temp_path;
    int fd = -1;
    const std::uint64_t write_started_us = now_us();
    for (int attempt = 0; attempt < 100; ++attempt) {
        temp_path = make_temp_path();
        fd = file_operations_->open_exclusive(temp_path, 0600);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            set_error("failed to create RDB temporary file");
            std::lock_guard<std::mutex> lock(result_mutex_);
            last_save_result_ = result;
            return result;
        }
    }
    if (fd < 0) {
        set_error("failed to create RDB temporary file");
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = result;
        return result;
    }

    if (!write_all(file_operations_.get(), fd, image.data(), image.size())) {
        const int saved_errno = errno;
        file_operations_->close_fd(fd);
        file_operations_->unlink_file(temp_path);
        errno = saved_errno;
        set_error("failed to write RDB snapshot");
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = result;
        return result;
    }
    if (timings != nullptr) {
        timings->write_us = now_us() - write_started_us;
    }

    const std::uint64_t fsync_started_us = now_us();
    if (file_operations_->fsync_fd(fd) < 0) {
        const int saved_errno = errno;
        file_operations_->close_fd(fd);
        file_operations_->unlink_file(temp_path);
        errno = saved_errno;
        set_error("RDB snapshot fsync failed");
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = result;
        return result;
    }

    if (file_operations_->close_fd(fd) < 0) {
        const int saved_errno = errno;
        file_operations_->unlink_file(temp_path);
        errno = saved_errno;
        set_error("RDB snapshot close failed");
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = result;
        return result;
    }

    if (file_operations_->rename_file(temp_path, rdb_path_) < 0) {
        const int saved_errno = errno;
        file_operations_->unlink_file(temp_path);
        errno = saved_errno;
        set_error("RDB snapshot rename failed");
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = result;
        return result;
    }

    int dir_fd = file_operations_->open_parent_dir(rdb_path_);
    if (dir_fd < 0) {
        result = RdbSaveResult::DurabilityUnconfirmed;
        set_error("RDB parent directory could not be opened");
    } else {
        const int fsync_result = file_operations_->fsync_dir(dir_fd);
        const int saved_errno = errno;
        file_operations_->close_fd(dir_fd);
        result = fsync_result == 0 ? RdbSaveResult::Ok
                                   : RdbSaveResult::DurabilityUnconfirmed;
        if (fsync_result < 0) {
            errno = saved_errno;
            set_error("RDB parent directory fsync failed");
        }
    }
    if (timings != nullptr) {
        timings->fsync_us = now_us() - fsync_started_us;
    }

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_save_result_ = result;
    }
    return result;
}

int RdbManager::load(KvStore& store) {
    std::error_code ec;
    const auto status = fs::status(rdb_path_, ec);
    if (ec) {
        // A missing path is the only status error that represents an empty
        // database.  Permission errors, ENAMETOOLONG, I/O errors and other
        // lookup failures must never be mistaken for an empty snapshot.
        if (ec == std::errc::no_such_file_or_directory) {
            return 0;
        }
        return -1;
    }
    if (status.type() == fs::file_type::not_found) {
        return 0;
    }
    if (!fs::is_regular_file(status)) {
        return -1;
    }

    const uintmax_t file_size_u = fs::file_size(rdb_path_, ec);
    if (ec || file_size_u > RdbFormat::MAX_FILE_SIZE) {
        return -1;
    }
    const size_t file_size = static_cast<size_t>(file_size_u);
    if (file_size < RdbFormat::MIN_FILE_SIZE) {
        return -1;
    }

    std::ifstream file(rdb_path_, std::ios::binary);
    if (!file.is_open()) {
        return -1;
    }
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()),
              static_cast<std::streamsize>(file_size));
    if (!file || file.peek() != std::char_traits<char>::eof()) {
        return -1;
    }
    file.close();

    const size_t record_end = file_size - RdbFormat::CHECKSUM_SIZE;
    uint32_t stored_checksum_net = 0;
    std::memcpy(&stored_checksum_net, buffer.data() + record_end, 4);
    const uint32_t stored_checksum = RdbFormat::ntoh32(stored_checksum_net);
    const uint32_t computed_checksum =
        RdbFormat::crc32c(buffer.data(), record_end);
    if (stored_checksum != computed_checksum ||
        std::memcmp(buffer.data(), RdbFormat::MAGIC, 4) != 0) {
        return -1;
    }

    uint32_t version_net = 0;
    std::memcpy(&version_net, buffer.data() + 4, 4);
    if (RdbFormat::ntoh32(version_net) != RdbFormat::VERSION) {
        return -1;
    }

    uint32_t count_net = 0;
    std::memcpy(&count_net, buffer.data() + 8, 4);
    const uint32_t declared_count = RdbFormat::ntoh32(count_net);
    if (declared_count > RdbFormat::MAX_RECORDS) {
        return -1;
    }

    KvStore temp_store(store.clock());
    std::set<std::string> seen_keys;
    size_t offset = RdbFormat::HEADER_SIZE;
    size_t recovery_bytes = 0;

    for (uint32_t record = 0; record < declared_count; ++record) {
        auto key_len_result = RdbFormat::decode_varint(buffer.data() + offset,
                                                       record_end - offset);
        if (key_len_result.second == 0) {
            return -1;
        }
        const uint32_t key_len = key_len_result.first;
        offset += key_len_result.second;
        if (key_len == 0 || key_len > RdbFormat::MAX_KEY_SIZE ||
            key_len > record_end - offset) {
            return -1;
        }
        std::string key(reinterpret_cast<const char*>(buffer.data() + offset),
                        key_len);
        offset += key_len;

        auto value_len_result = RdbFormat::decode_varint(buffer.data() + offset,
                                                         record_end - offset);
        if (value_len_result.second == 0) {
            return -1;
        }
        const uint32_t value_len = value_len_result.first;
        offset += value_len_result.second;
        if (value_len > RdbFormat::MAX_VALUE_SIZE ||
            value_len > record_end - offset) {
            return -1;
        }
        std::string value(reinterpret_cast<const char*>(buffer.data() + offset),
                          value_len);
        offset += value_len;

        if (record_end - offset < 8) {
            return -1;
        }
        uint64_t expiry_net = 0;
        std::memcpy(&expiry_net, buffer.data() + offset, 8);
        const int64_t expiry_ms =
            static_cast<int64_t>(RdbFormat::ntoh64(expiry_net));
        offset += 8;
        if (expiry_ms < 0) {
            return -1;
        }

        if (!seen_keys.insert(key).second) {
            return -1;
        }
        recovery_bytes += key.size() + value.size();
        if (recovery_bytes > RdbFormat::MAX_RECOVERY_BYTES) {
            return -1;
        }
        if (!temp_store.restore_entry(key, value, expiry_ms)) {
            return -1;
        }
    }

    if (offset != record_end) {
        return -1;
    }

    store.swap(temp_store);
    return static_cast<int>(declared_count);
}

} // namespace minidb
