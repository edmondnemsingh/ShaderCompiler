#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sp {

// 64-bit content hash (XXH3). Used to dedupe shader/root-signature blobs and to identify PSOs.
uint64_t hash64(const void* data, size_t size, uint64_t seed = 0);

std::string hex64(uint64_t v);

}  // namespace sp
