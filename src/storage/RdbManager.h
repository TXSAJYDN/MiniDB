#pragma once
#include "KvStore.h"
#include "RdbFormat.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>

namespace minidb {

enum class RdbSaveResult : std::uint8_t {
    Ok,
    Failed,
    // rename() succeeded, but the parent directory fsync failed.  The new
    // snapshot is visible but its crash durability cannot be confirmed.
    DurabilityUnconfirmed,
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

    std::string rdb_path_;
    std::shared_ptr<RdbFileOperations> file_operations_;
    mutable std::mutex save_mutex_;
    mutable std::mutex result_mutex_;
    RdbSaveResult last_save_result_ = RdbSaveResult::Ok;
};

} // namespace minidb
