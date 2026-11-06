# Dependencies

## Runtime and production code

MiniDB's production code uses only the C++20 standard library and Linux/POSIX
interfaces. It does not link a network framework, HTTP library, database, JSON
library, logging framework, or compression library.

## Build and test dependencies

| Dependency | Version / source | Scope | Why |
|---|---|---|---|
| CMake | 3.14+ | Build | Presets, targets, CTest integration |
| GCC or Clang | C++20-capable | Build | Linux implementation and sanitizers |
| GoogleTest | v1.14.0 via FetchContent, pinned SHA-256 | Tests only | Unit/integration test framework and discovery |
| Python 3 | 3.10+ expected | Process tests, benchmarks, tools | Standard-library-only harnesses |
| Clang/libFuzzer | Clang 18 verified | Fuzz targets only | Coverage-guided parser testing |
| strace, perf | Distribution packages | Diagnostics only | Syscall tracing and limited WSL2 sampling |
| clang-format, clang-tidy | Clang 18 verified | Development only | Formatting and static analysis |

GoogleTest is the only third-party source code built by CMake. Its pinned archive
hash is recorded in `CMakeLists.txt`. The local acceptance workflow supplies an
existing GoogleTest source tree through
`FETCHCONTENT_SOURCE_DIR_GOOGLETEST` so a clean build does not depend on network
availability.

## Licensing note

No repository-level license file is currently present. Distribution or
public-contribution terms therefore require an explicit owner decision; this
document does not infer or grant a license.
