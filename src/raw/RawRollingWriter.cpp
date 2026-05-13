#include "cambuffer_recorder_ng/raw/RawRollingWriter.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

namespace cambuffer_recorder_ng
{

uint64_t systemUtcNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool RawRollingWriter::open(const RawRollingWriterConfig& config)
{
    close();
    config_ = config;
    if (config_.run_start_utc_ns == 0) config_.run_start_utc_ns = systemUtcNowNs();
    if (config_.source_stride_bytes == 0) config_.source_stride_bytes = config_.width;
    if (config_.roll_bytes == 0) config_.roll_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    file_index_ = 0;
    bytes_in_file_ = 0;
    files_written_.clear();
    return openNewFile();
}

bool RawRollingWriter::openNewFile()
{
    if (fp_) {
        fflush(fp_);
        fclose(fp_);
        fp_ = nullptr;
    }

    std::ostringstream name;
    name << config_.path_prefix << "_";
    name.width(4);
    name.fill('0');
    name << file_index_ << ".cbrraw";
    const std::string filename = name.str();

    fp_ = fopen(filename.c_str(), "wb");
    if (!fp_) {
        perror("fopen rolling raw file");
        return false;
    }

    RawRollingFileHeader fh{};
    fh.magic = CBRRAW_FILE_MAGIC;
    fh.version = CBRRAW_VERSION;
    fh.header_size = sizeof(RawRollingFileHeader);
    fh.file_index = file_index_;
    fh.pixel_format = static_cast<uint32_t>(config_.pixel_format);
    fh.run_start_utc_ns = config_.run_start_utc_ns;
    fh.file_start_utc_ns = systemUtcNowNs();
    fh.width = config_.width;
    fh.height = config_.height;
    fh.source_stride_bytes = config_.source_stride_bytes;
    fh.packed_stride_bytes = config_.width;

    if (fwrite(&fh, 1, sizeof(fh), fp_) != sizeof(fh)) {
        perror("fwrite rolling file header");
        close();
        return false;
    }

    bytes_in_file_ = sizeof(fh);
    files_written_.push_back(filename);
    std::cerr << "[raw-roll] opened " << filename << " (" << config_.width << "x" << config_.height
              << ", source stride " << config_.source_stride_bytes << ")\n";
    ++file_index_;
    return true;
}

bool RawRollingWriter::maybeRoll(uint64_t next_record_bytes)
{
    if (!fp_) return false;
    if (bytes_in_file_ + next_record_bytes > config_.roll_bytes) {
        return openNewFile();
    }
    return true;
}

bool RawRollingWriter::writeFrame(uint64_t frame_index,
                                  uint64_t pc_utc_ns,
                                  uint64_t camera_timestamp_ns,
                                  const uint8_t* data,
                                  uint32_t payload_bytes)
{
    if (!fp_ || !data) return false;

    const uint64_t record_bytes = sizeof(RawRollingFrameHeader) + payload_bytes;
    if (!maybeRoll(record_bytes)) return false;

    RawRollingFrameHeader fh{};
    fh.magic = CBRRAW_FRAME_MAGIC;
    fh.version = CBRRAW_VERSION;
    fh.header_size = sizeof(RawRollingFrameHeader);
    fh.header_flags = 0;
    fh.frame_index = frame_index;
    fh.pc_utc_ns = pc_utc_ns;
    fh.camera_timestamp_ns = camera_timestamp_ns;
    fh.width = config_.width;
    fh.height = config_.height;
    fh.source_stride_bytes = config_.source_stride_bytes;
    fh.payload_bytes = payload_bytes;
    fh.pixel_format = static_cast<uint32_t>(config_.pixel_format);

    if (fwrite(&fh, 1, sizeof(fh), fp_) != sizeof(fh)) {
        perror("fwrite rolling frame header");
        return false;
    }
    if (fwrite(data, 1, payload_bytes, fp_) != payload_bytes) {
        perror("fwrite rolling frame payload");
        return false;
    }

    bytes_in_file_ += record_bytes;
    return true;
}

void RawRollingWriter::close()
{
    if (fp_) {
        fflush(fp_);
        fclose(fp_);
        fp_ = nullptr;
    }
}

}  // namespace cambuffer_recorder_ng
