#pragma once

#include "cambuffer_recorder_ng/raw/RawRollingFormat.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace cambuffer_recorder_ng
{

struct RawRollingWriterConfig
{
    std::string path_prefix;
    uint64_t roll_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_stride_bytes = 0;
    RawPixelFormat pixel_format = RawPixelFormat::RAW8_BAYER_GBRG;
    uint64_t run_start_utc_ns = 0;
};

class RawRollingWriter
{
public:
    RawRollingWriter() = default;
    ~RawRollingWriter() { close(); }

    bool open(const RawRollingWriterConfig& config);
    bool writeFrame(uint64_t frame_index,
                    uint64_t pc_utc_ns,
                    uint64_t camera_timestamp_ns,
                    uint64_t camera_frame_number,
                    const uint8_t* data,
                    uint32_t payload_bytes);
    void close();

    // Called immediately before opening a new rollover file. Return false to stop
    // the rollover cleanly before creating another file. The argument is the
    // zero-based file index that would be opened next.
    using RolloverCallback = std::function<bool(uint32_t next_file_index)>;
    void setRolloverCallback(RolloverCallback callback) { rollover_callback_ = std::move(callback); }

    uint32_t currentFileIndex() const { return file_index_; }
    uint64_t bytesInFile() const { return bytes_in_file_; }
    const std::vector<std::string>& filesWritten() const { return files_written_; }

private:
    bool openNewFile();
    bool maybeRoll(uint64_t next_record_bytes);

    RawRollingWriterConfig config_;
    FILE* fp_{nullptr};
    uint32_t file_index_{0};
    uint64_t bytes_in_file_{0};
    std::vector<std::string> files_written_;
    RolloverCallback rollover_callback_;
};

uint64_t systemUtcNowNs();

}  // namespace cambuffer_recorder_ng
