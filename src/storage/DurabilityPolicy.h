#pragma once

#include <cstdint>

namespace minidb {

// R1 intentionally implements only the two modes whose semantics are covered
// by the crash matrix.  BATCHED remains an evidence-driven R4 option.
enum class DurabilityMode : std::uint8_t {
    // WAL append and fsync complete before the mutation is applied and
    // acknowledged.  This is the default and the only mode that promises
    // "acknowledged writes survive an arbitrary process crash".
    kSync = 0,
    // WAL append completes before the mutation is acknowledged; fsync is
    // driven by the owner EventLoop timer and before checkpoint/shutdown.
    // The allowed loss window is therefore explicit and measurable.
    kInterval = 1,
};

inline const char* durability_mode_name(DurabilityMode mode) {
    switch (mode) {
    case DurabilityMode::kSync:
        return "sync";
    case DurabilityMode::kInterval:
        return "interval";
    }
    return "unknown";
}

} // namespace minidb
