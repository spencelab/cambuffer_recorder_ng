#pragma once

#include <cstdint>

namespace cambuffer_recorder_ng
{

static constexpr uint32_t CBRRAW_FILE_MAGIC  = 0x52524243; // 'CBRR' little-endian
static constexpr uint32_t CBRRAW_FRAME_MAGIC = 0x46524243; // 'CBRF' little-endian
static constexpr uint16_t CBRRAW_VERSION = 1;

enum class RawPixelFormat : uint32_t
{
    RAW8_BAYER_GBRG = 1,
    RAW8_MONO = 2,
};

#pragma pack(push, 1)
struct RawRollingFileHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t file_index;
    uint32_t pixel_format;
    uint64_t run_start_utc_ns;
    uint64_t file_start_utc_ns;
    uint32_t width;
    uint32_t height;
    uint32_t source_stride_bytes;
    uint32_t packed_stride_bytes;
    uint64_t reserved0;
    uint64_t reserved1;
};

struct RawRollingFrameHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t header_flags;
    uint16_t reserved0;
    uint64_t frame_index;
    uint64_t pc_utc_ns;
    uint64_t camera_timestamp_ns;
    uint32_t width;
    uint32_t height;
    uint32_t source_stride_bytes;
    uint32_t payload_bytes;
    uint32_t pixel_format;
    uint32_t reserved1;
};
#pragma pack(pop)

}  // namespace cambuffer_recorder_ng
