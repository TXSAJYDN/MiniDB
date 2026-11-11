add_library(minidb_base STATIC
    FileDescriptor.cpp
    Buffer.cpp
    Clock.cpp
    Sha256.cpp
    Log.cpp
)

target_include_directories(minidb_base PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
