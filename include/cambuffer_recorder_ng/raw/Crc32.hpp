#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cambuffer_recorder_ng
{

inline uint32_t crc32Ieee(const uint8_t* data, size_t size)
{
    static const std::array<uint32_t, 256> table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFFU] ^ (c >> 8U);
    }
    return c ^ 0xFFFFFFFFU;
}

}  // namespace cambuffer_recorder_ng
