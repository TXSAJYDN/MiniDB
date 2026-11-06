#ifndef MINIDB_BASE_INT128_H
#define MINIDB_BASE_INT128_H

// GCC/Clang provide a 128-bit integer type as a documented extension.  Keep
// the extension spelling in one place so -Wpedantic -Werror remains useful for
// the rest of the codebase.
#if !defined(__SIZEOF_INT128__)
#error "miniDB requires a compiler with a 128-bit integer type"
#endif

namespace minidb {
__extension__ typedef __int128 int128_t;
} // namespace minidb

#endif // MINIDB_BASE_INT128_H
