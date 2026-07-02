#pragma once

#include <cstdint>
#include <cstdio>
#include <istream>
#include <string>

namespace cambuffer_recorder_ng
{

static constexpr uint32_t CBRRAW_FILE_MAGIC  = 0x52524243; // 'CBRR' little-endian
static constexpr uint32_t CBRRAW_FRAME_MAGIC = 0x46524243; // 'CBRF' little-endian
static constexpr uint16_t CBRRAW_VERSION = 2;
// Version 1 frame header reserved1 was repurposed as camera_frame_number.
// Version 2 appends payload_crc32 to each frame header. Readers should honor
// header_size so v1 and v2 files remain readable.
static constexpr uint16_t CBRRAW_MIN_SUPPORTED_VERSION = 1;
static constexpr uint16_t CBRRAW_FRAME_FLAG_PAYLOAD_CRC32 = 1U << 0U;

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

struct RawRollingFrameHeaderV1
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
    uint32_t camera_frame_number;  // XIMEA XI_IMG.nframe when available; 0 means unknown
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
    uint32_t camera_frame_number;  // XIMEA XI_IMG.nframe when available; 0 means unknown
    uint32_t payload_crc32;        // IEEE CRC32 over payload when header_flags bit 0 is set
};
#pragma pack(pop)

inline void upgradeRawRollingFrameHeader(const RawRollingFrameHeaderV1& v1,
                                         RawRollingFrameHeader& out)
{
    out.magic = v1.magic;
    out.version = v1.version;
    out.header_size = v1.header_size;
    out.header_flags = v1.header_flags;
    out.reserved0 = v1.reserved0;
    out.frame_index = v1.frame_index;
    out.pc_utc_ns = v1.pc_utc_ns;
    out.camera_timestamp_ns = v1.camera_timestamp_ns;
    out.width = v1.width;
    out.height = v1.height;
    out.source_stride_bytes = v1.source_stride_bytes;
    out.payload_bytes = v1.payload_bytes;
    out.pixel_format = v1.pixel_format;
    out.camera_frame_number = v1.camera_frame_number;
    out.payload_crc32 = 0;
}

inline bool readRawRollingFrameHeader(FILE* fp,
                                      RawRollingFrameHeader& out,
                                      bool& eof,
                                      std::string& error)
{
    eof = false;
    error.clear();

    RawRollingFrameHeaderV1 v1{};
    const size_t got = std::fread(&v1, 1, sizeof(v1), fp);
    if (got == 0) {
        eof = true;
        return false;
    }
    if (got != sizeof(v1)) {
        error = "partial frame header";
        return false;
    }
    upgradeRawRollingFrameHeader(v1, out);

    if (out.magic != CBRRAW_FRAME_MAGIC) {
        error = "bad frame magic";
        return false;
    }
    if (out.header_size < sizeof(RawRollingFrameHeaderV1)) {
        error = "frame header_size is smaller than v1 header";
        return false;
    }

    size_t remaining = static_cast<size_t>(out.header_size) - sizeof(RawRollingFrameHeaderV1);
    if (remaining >= sizeof(uint32_t)) {
        uint32_t crc = 0;
        if (std::fread(&crc, 1, sizeof(crc), fp) != sizeof(crc)) {
            error = "partial extended frame header";
            return false;
        }
        out.payload_crc32 = crc;
        remaining -= sizeof(crc);
    }
    if (remaining > 0 && std::fseek(fp, static_cast<long>(remaining), SEEK_CUR) != 0) {
        error = "could not skip unknown frame header extension";
        return false;
    }
    return true;
}

inline bool readRawRollingFrameHeader(std::istream& in,
                                      RawRollingFrameHeader& out,
                                      bool& eof,
                                      std::string& error)
{
    eof = false;
    error.clear();

    RawRollingFrameHeaderV1 v1{};
    in.read(reinterpret_cast<char*>(&v1), static_cast<std::streamsize>(sizeof(v1)));
    const auto got = in.gcount();
    if (got == 0) {
        eof = true;
        return false;
    }
    if (got != static_cast<std::streamsize>(sizeof(v1))) {
        error = "partial frame header";
        return false;
    }
    upgradeRawRollingFrameHeader(v1, out);

    if (out.magic != CBRRAW_FRAME_MAGIC) {
        error = "bad frame magic";
        return false;
    }
    if (out.header_size < sizeof(RawRollingFrameHeaderV1)) {
        error = "frame header_size is smaller than v1 header";
        return false;
    }

    size_t remaining = static_cast<size_t>(out.header_size) - sizeof(RawRollingFrameHeaderV1);
    if (remaining >= sizeof(uint32_t)) {
        uint32_t crc = 0;
        in.read(reinterpret_cast<char*>(&crc), static_cast<std::streamsize>(sizeof(crc)));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(crc))) {
            error = "partial extended frame header";
            return false;
        }
        out.payload_crc32 = crc;
        remaining -= sizeof(crc);
    }
    if (remaining > 0) {
        in.seekg(static_cast<std::streamoff>(remaining), std::ios::cur);
        if (!in) {
            error = "could not skip unknown frame header extension";
            return false;
        }
    }
    return true;
}

}  // namespace cambuffer_recorder_ng
