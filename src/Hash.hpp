// FNV-1a 64-bit content hashing for index staleness / prefix / bit-identity checks.
//
// NOTE(deviation from plan): the plan text names sha256; we use FNV-1a-64 because the hash's
// only job here is change-detection (staleness), append-prefix detection (incremental build),
// and bit-identical-rebuild assertions — not cryptographic integrity. FNV-1a is deterministic,
// dependency-free, and sufficient for all three. If a cryptographic guarantee is ever needed
// (e.g. signed shards), swap this one function for SHA-256; nothing else changes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace kelvin {

inline constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
inline constexpr uint64_t kFnvPrime = 1099511628211ULL;

inline uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = kFnvOffset) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

inline uint64_t fnv1a64(const std::string& s, uint64_t seed = kFnvOffset) {
    return fnv1a64(s.data(), s.size(), seed);
}

}  // namespace kelvin
