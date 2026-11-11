#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace minidb {

// Small dependency-free SHA-256 implementation used by the Agent protocol
// for canonical request fingerprints.  This is cryptographic hashing, not a
// password hash, and is intentionally limited to the fixed digest operation.
class Sha256 {
  public:
    using Digest = std::array<std::uint8_t, 32>;

    static Digest digest(std::string_view input);
};

} // namespace minidb
