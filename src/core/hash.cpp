#include "core/hash.hpp"

#include <cstdio>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace sp {

uint64_t hash64(const void* data, size_t size, uint64_t seed) {
    return XXH3_64bits_withSeed(data, size, seed);
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace sp
