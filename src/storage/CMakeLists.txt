add_library(minidb_storage
    KvStore.cpp
    RdbFormat.cpp
    RdbManager.cpp
)

target_include_directories(minidb_storage PUBLIC)
target_link_libraries(minidb_storage PUBLIC minidb_base)
