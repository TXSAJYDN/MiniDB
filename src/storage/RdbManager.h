#pragma once
#include "KvStore.h"
#include "RdbFormat.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <vector>

namespace minidb {

enum class RdbSaveResult : std::uint8_t {
    Ok,
    Failed,
    // rename() succeeded, but the parent directory fsync failed.  The new
    // snapshot is visible but its crash durability cannot be confirmed.
    DurabilityUnconfirmed,
};

// An immutable copy of the live store at one checkpoint boundary.  Capture
// owns independent key/value strings, so serialization and durable writes can
// run on a worker thread without holding the KvStore scanner pause.
struct RdbSnapshot {
    std::vector<KvStore::Entry> entries;
};

struct RdbSaveTimings {
    std::uint64_t serialize_us = 0;
    std::uint64_t write_us = 0;
    std::uint64_t fsync_us = 0;
};

// Narrow seam used to inject file-system failures in tests.  The production
// implementation performs direct POSIX calls.
class RdbFileOperations {
  public:
    virtual ~RdbFileOperations() = default;
    virtual int open_exclusive(const std::string& path, mode_t mode) = 0;
    virtual ssize_t write(int fd, const void* data, size_t len) = 0;
    virtual int fsync_fd(int fd) = 0;
    virtual int close_fd(int fd) = 0;
    virtual int rename_file(const std::string& from, const std::string& to) = 0;
    virtual int unlink_file(const std::string& path) = 0;
    virtual int open_parent_dir(const std::string& path) = 0;
    virtual int fsync_dir(int fd) = 0;
};

class RdbManager {
  public:
    explicit RdbManager(const std::string& rdb_path);
    RdbManager(const std::string& rdb_path,
               std::shared_ptr<RdbFileOperations> file_operations);
    ~RdbManager();

    RdbManager(const RdbManager&) = delete;
    RdbManager& operator=(const RdbManager&) = delete;

    // Serializes concurrent saves.  Returns true only when the snapshot was
    // atomically replaced and the parent directory was durably synced.
    bool save(const KvStore& store);
    RdbSaveResult save_checked(const KvStore& store);
    // Checkpoint callers hold one scanner pause across WAL rotation, snapshot
    // publication and WAL cleanup.  This overload reuses that pause instead
    // of recursively acquiring another scanner pause.
    RdbSaveResult save_checked(const KvStore& store,
                               KvStore::ScannerPause& scanner_pause);

    // Background-checkpoint pipeline.  capture() is the only step that needs
    // the store scanner paused; serialize() and write_durable() operate on an
    // independent snapshot and may run on a worker thread.
    bool capture(const KvStore& store, RdbSnapshot* snapshot,
                 std::string* error = nullptr);
    bool capture(const KvStore& store, KvStore::ScannerPause& scanner_pause,
                 RdbSnapshot* snapshot, std::string* error = nullptr);
    bool serialize(const RdbSnapshot& snapshot,
                   std::vector<std::uint8_t>* image,
                   RdbSaveTimings* timings = nullptr,
                   std::string* error = nullptr);
    RdbSaveResult write_durable(const std::vector<std::uint8_t>& image,
                                RdbSaveTimings* timings = nullptr,
                                std::string* error = nullptr);

    // Fully validates and builds a temporary store, then atomically swaps it
    // into the destination.  On failure the destination is unchanged.
    // Returns the number of records, or -1 on corruption/resource-limit error.
    int load(KvStore& store);

    bool exists() const;
    bool remove();

    RdbSaveResult last_save_result() const;

  private:
    std::string make_temp_path() const;
    RdbSaveResult save_checked_under_lock(const KvStore& store);
    static bool capture_entries(const KvStore& store, RdbSnapshot* snapshot,
                                std::string* error);
    static bool serialize_entries(const RdbSnapshot& snapshot,
                                  std::vector<std::uint8_t>* image,
                                  std::string* error);
    RdbSaveResult
    write_durable_under_lock(const std::vector<std::uint8_t>& image,
                             RdbSaveTimings* timings, std::string* error);

    std::string rdb_path_;
    std::shared_ptr<RdbFileOperations> file_operations_;
    mutable std::mutex save_mutex_;
    mutable std::mutex result_mutex_;
    RdbSaveResult last_save_result_ = RdbSaveResult::Ok;
};

} // namespace minidb
