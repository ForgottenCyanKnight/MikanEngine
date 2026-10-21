#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

namespace mikan::rt {
// Ordered geometry only. Appearance deliberately remains owned by each surface.
// The full words are compared after hashing: a hash collision never aliases BLAS.
struct VoxGeometryKey {
    std::vector<uint32_t> words;
    uint64_t hash=1469598103934665603ull;
    bool operator==(const VoxGeometryKey& other)const{return words==other.words;}
};
inline VoxGeometryKey MakeVoxGeometryKey(std::span<const uint32_t> quads,
    std::span<const uint32_t> planes,std::span<const uint32_t> ranges,
    const std::array<float,4>& minAndSize){
    VoxGeometryKey result;
    result.words.reserve(8+quads.size()+planes.size()+ranges.size());
    result.words.push_back(1); // Geometry contract version, not an asset format.
    for(float value:minAndSize)result.words.push_back(std::bit_cast<uint32_t>(value));
    for(auto section:{quads,planes,ranges}){
        result.words.push_back(uint32_t(section.size()));
        result.words.insert(result.words.end(),section.begin(),section.end());
    }
    for(uint32_t word:result.words)for(uint32_t byte=0;byte<4;++byte){
        result.hash^=(word>>(byte*8))&255u;result.hash*=1099511628211ull;
    }
    return result;
}
}
