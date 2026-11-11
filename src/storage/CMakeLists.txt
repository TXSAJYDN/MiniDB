add_library(minidb_storage
    KvStore.cpp
    RdbFormat.cpp
    RdbManager.cpp
    WalFormat.cpp
    WalFileOperations.cpp
    CheckpointManifest.cpp
    SingleWriterLock.cpp
    WalManager.cpp
    GroupCommitWriter.cpp
    WalReplay.cpp
    RecoveryManager.cpp
    PersistenceManager.cpp
)

target_include_directories(minidb_storage PUBLIC)
target_link_libraries(minidb_storage PUBLIC minidb_base)
