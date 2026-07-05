// Manufacturer policy for selection — two opt-in controls on the candidate set:
//   * allowlist filter: only rows from named manufacturers are considered;
//   * diversity cap:    no single manufacturer exceeds max_frac of the output,
//                       so the returned Pareto spans many vendors.
// Both default to no-op (empty allowlist, max_frac >= 1.0) so the selector stays
// parity-locked to the Python selector unless the caller opts in (a setting today,
// GUI-wired later). Applied to the RANKED list before truncation to max_candidates.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace kelvin {

struct MfrPolicy {
    std::vector<std::string> allowlist;  // normalized names; empty = allow all
    double max_frac = 1.0;               // per-manufacturer cap fraction; >=1 = no cap
    bool active() const { return !allowlist.empty() || max_frac < 1.0; }
};

// Fold a Latin-1-supplement (U+00C0..U+00FF, UTF-8 0xC3 0x80..0xBF) accented
// letter to its ASCII base, so "Würth" and "Wurth" normalise alike (mirrors the
// Python _normalize_manufacturer accent strip). Returns 0 if not a mapped char.
inline char _fold_latin1(unsigned char b2) {
    // b2 is the 2nd UTF-8 byte after 0xC3; low nibble ignores case (0x80 vs 0xA0).
    unsigned char lo = static_cast<unsigned char>(0x80 + (b2 & 0x1F));
    switch (lo) {
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: return 'a';
        case 0x87: return 'c';
        case 0x88: case 0x89: case 0x8A: case 0x8B: return 'e';
        case 0x8C: case 0x8D: case 0x8E: case 0x8F: return 'i';
        case 0x91: return 'n';
        case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: return 'o';
        case 0x99: case 0x9A: case 0x9B: case 0x9C: return 'u';
        case 0x9D: return 'y';
        default: return 0;
    }
}

// Lowercase + trim + fold Latin-1 accents, so an allowlist entry matches a
// catalogue name regardless of accent/case. Matching is substring-based
// ("wurth" matches "würth elektronik").
inline std::string norm_mfr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {  // 2-byte Latin-1-supplement letter
            char base = _fold_latin1(static_cast<unsigned char>(s[i + 1]));
            if (base) {
                out += base;
                ++i;
                continue;
            }
        }
        out += static_cast<char>(std::tolower(c));
    }
    size_t a = out.find_first_not_of(" \t");
    size_t b = out.find_last_not_of(" \t");
    return (a == std::string::npos) ? "" : out.substr(a, b - a + 1);
}

inline bool mfr_allowed(const std::string& manufacturer, const MfrPolicy& p) {
    if (p.allowlist.empty()) return true;
    const std::string m = norm_mfr(manufacturer);
    for (const std::string& allowed : p.allowlist)
        if (!allowed.empty() && m.find(allowed) != std::string::npos) return true;
    return false;
}

// Apply BOTH manufacturer controls to an ALREADY-SORTED (best-first) candidate
// list and return up to max_candidates row pointers to emit:
//   1. allowlist  — keep only rows from allowed manufacturers (ranked order);
//   2. diversity  — no manufacturer exceeds floor(max_frac · N) of the output,
//      greedy best-first, then fill any shortfall from the best deferred rows so
//      the caller still gets up to N (diversity-preferring, never fewer than a
//      plain top-N of the allowed set).
// A no-op policy returns the plain top-N — identical to the pre-feature behaviour.
template <class Row>
std::vector<const Row*> apply_mfr_policy(const std::vector<const Row*>& sorted,
                                        size_t max_candidates, const MfrPolicy& p) {
    // 1. allowlist filter (ranked order preserved).
    const std::vector<const Row*>* src = &sorted;
    std::vector<const Row*> allowed;
    if (!p.allowlist.empty()) {
        for (const Row* r : sorted)
            if (mfr_allowed(r->manufacturer, p)) allowed.push_back(r);
        src = &allowed;
    }
    const size_t n = std::min(max_candidates, src->size());
    if (p.max_frac >= 1.0 || n == 0) return {src->begin(), src->begin() + n};
    // 2. diversity cap.
    const size_t cap = std::max<size_t>(1, static_cast<size_t>(std::floor(p.max_frac * static_cast<double>(n))));
    std::vector<const Row*> out, deferred;
    out.reserve(n);
    std::unordered_map<std::string, size_t> count;
    for (const Row* r : *src) {
        if (out.size() >= n) break;
        const std::string m = norm_mfr(r->manufacturer);
        if (count[m] < cap) {
            out.push_back(r);
            ++count[m];
        } else {
            deferred.push_back(r);
        }
    }
    for (const Row* r : deferred) {
        if (out.size() >= n) break;
        out.push_back(r);
    }
    return out;
}

}  // namespace kelvin
