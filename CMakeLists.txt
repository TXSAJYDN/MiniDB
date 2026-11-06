cmake_minimum_required(VERSION 3.14)
project(MiniDB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Sanitizer support. ASan and TSan cannot be combined in one process.
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_FUZZERS "Build Clang libFuzzer targets" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()

set(MINIDB_SANITIZERS "")
if(ENABLE_ASAN)
    list(APPEND MINIDB_SANITIZERS address)
endif()
if(ENABLE_UBSAN)
    list(APPEND MINIDB_SANITIZERS undefined)
endif()
if(ENABLE_TSAN)
    list(APPEND MINIDB_SANITIZERS thread)
endif()

if(MINIDB_SANITIZERS)
    # Keep one -fsanitize=<name> option per sanitizer.  Besides being accepted
    # by GCC and Clang, this makes acceptance checks and compiler diagnostics
    # show address and undefined independently in combined builds.
    set(MINIDB_SANITIZER_FLAGS "")
    set(MINIDB_SANITIZER_LINK_FLAGS "")
    foreach(sanitizer IN LISTS MINIDB_SANITIZERS)
        string(APPEND MINIDB_SANITIZER_FLAGS " -fsanitize=${sanitizer}")
        string(APPEND MINIDB_SANITIZER_LINK_FLAGS " -fsanitize=${sanitizer}")
    endforeach()
    string(APPEND MINIDB_SANITIZER_FLAGS
        " -fno-omit-frame-pointer -fno-sanitize-recover=all")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}${MINIDB_SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS}${MINIDB_SANITIZER_LINK_FLAGS}")
endif()

if(ENABLE_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ENABLE_FUZZERS requires Clang (libFuzzer)")
endif()

if(ENABLE_FUZZERS)
    # Instrument every project source, not only the fuzz harness.  Existing
    # executables still need a sanitizer runtime when this configuration is
    # built, so fuzzer-no-link is also part of the executable link flags.  The
    # fuzz targets add -fsanitize=fuzzer themselves to obtain libFuzzer's main.
    set(MINIDB_FUZZ_COMPILE_FLAGS
        "-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MINIDB_FUZZ_COMPILE_FLAGS}")
endif()

# GoogleTest
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

if(ENABLE_FUZZERS)
    # C++ objects are instrumented above.  Every executable defined below also
    # needs the sanitizer runtime; fuzz targets additionally select libFuzzer.
    add_link_options(-fsanitize=fuzzer-no-link,address,undefined)
endif()

enable_testing()
include(GoogleTest)

add_subdirectory(src/base)
add_subdirectory(tests)
add_subdirectory(experiments)

add_subdirectory(src/net)
add_subdirectory(src/protocol)
add_subdirectory(src/storage)
add_subdirectory(app)

if(ENABLE_FUZZERS)
    add_subdirectory(fuzz)
endif()
