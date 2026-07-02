#include "cambuffer_recorder_ng/raw/RawRollingFormat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cambuffer_recorder_ng;

namespace
{

struct FrameRecord
{
    uint64_t global_row = 0;
    std::string source_file;
    uint32_t file_index = 0;
    uint64_t file_frame_ordinal = 0;
    uint64_t frame_index = 0;
    uint64_t pc_utc_ns = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t camera_frame_number = 0;

    bool file_rollover_boundary = false;

    uint64_t delta_pc_ns = 0;
    double delta_pc_ms = 0.0;
    int64_t pc_delta_frames_est = 0;
    int64_t suspected_pc_missed_triggers = 0;

    uint64_t delta_camera_ns = 0;
    double delta_camera_ms = 0.0;
    int64_t camera_delta_frames_est = 0;
    int64_t suspected_camera_missed_triggers = 0;

    int64_t frame_index_gap = 0;
    int64_t camera_frame_number_gap = 0;
    std::string flag;
};

struct FileSummary
{
    std::string path;
    uint32_t file_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_stride_bytes = 0;
    uint32_t packed_stride_bytes = 0;
    uint32_t pixel_format = 0;
    uint64_t run_start_utc_ns = 0;
    uint64_t file_start_utc_ns = 0;
    uint64_t frames = 0;
    uint64_t first_frame_index = 0;
    uint64_t last_frame_index = 0;
    uint64_t first_pc_utc_ns = 0;
    uint64_t last_pc_utc_ns = 0;
    uint64_t first_camera_timestamp_ns = 0;
    uint64_t last_camera_timestamp_ns = 0;
    uint64_t first_camera_frame_number = 0;
    uint64_t last_camera_frame_number = 0;
};

struct DeltaStats
{
    uint64_t count = 0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    double std_ms = 0.0;
};

struct AuditSummary
{
    double expected_fps = 0.0;
    uint64_t expected_interval_ns = 0;
    double threshold_frames = 1.5;
    uint64_t files_seen = 0;
    uint64_t total_frames = 0;
    uint64_t first_pc_utc_ns = 0;
    uint64_t last_pc_utc_ns = 0;
    uint64_t first_camera_timestamp_ns = 0;
    uint64_t last_camera_timestamp_ns = 0;
    uint64_t first_camera_frame_number = 0;
    uint64_t last_camera_frame_number = 0;
    uint64_t first_frame_index = 0;
    uint64_t last_frame_index = 0;
    double duration_pc_s = 0.0;
    double duration_camera_s = 0.0;
    double mean_fps_by_pc_utc = 0.0;
    double mean_fps_by_camera_ts = 0.0;

    DeltaStats pc_delta;
    DeltaStats camera_delta;

    uint64_t pc_suspicious_intervals = 0;
    uint64_t pc_suspected_missed_triggers = 0;
    uint64_t camera_suspicious_intervals = 0;
    uint64_t camera_suspected_missed_triggers = 0;
    uint64_t camera_timestamps_seen = 0;
    uint64_t camera_timestamp_pairs = 0;
    uint64_t camera_timestamp_nonmonotonic = 0;

    uint64_t camera_frame_numbers_seen = 0;
    uint64_t camera_frame_number_pairs = 0;
    uint64_t camera_frame_number_gaps = 0;
    uint64_t total_camera_frame_number_gap = 0;
    uint64_t camera_frame_number_nonmonotonic = 0;
    uint64_t camera_frame_number_gaps_at_rollover = 0;

    uint64_t frame_index_gaps = 0;
    uint64_t total_frame_index_gap = 0;

    uint64_t file_rollover_boundaries = 0;
    uint64_t pc_suspicious_at_rollover = 0;
    uint64_t camera_suspicious_at_rollover = 0;
};

std::string pixelFormatName(uint32_t fmt)
{
    switch (static_cast<RawPixelFormat>(fmt)) {
        case RawPixelFormat::RAW8_BAYER_GBRG: return "RAW8_BAYER_GBRG";
        case RawPixelFormat::RAW8_MONO: return "RAW8_MONO";
        default: return "UNKNOWN(" + std::to_string(fmt) + ")";
    }
}

std::string utcNsToIso(uint64_t ns)
{
    if (ns == 0) return "";

    const std::time_t sec = static_cast<std::time_t>(ns / 1000000000ULL);
    const uint64_t nsec = ns % 1000000000ULL;

    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &sec);
#else
    gmtime_r(&sec, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setw(9) << std::setfill('0') << nsec << "Z";
    return oss.str();
}

bool fileExists(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

std::string makeIndexedPath(const std::string& first_path, int index)
{
    // Handles names like /tmp/run_20260518T000000Z_0000.cbrraw.
    static const std::regex re(R"((.*_)([0-9]{4})(\.cbrraw)$)");
    std::smatch m;
    if (!std::regex_match(first_path, m, re)) {
        return index == 0 ? first_path : std::string{};
    }

    std::ostringstream oss;
    oss << m[1].str() << std::setw(4) << std::setfill('0') << index << m[3].str();
    return oss.str();
}

bool readExact(std::ifstream& in, void* dst, std::streamsize n)
{
    in.read(reinterpret_cast<char*>(dst), n);
    return in.gcount() == n;
}

void appendFlag(std::string& flags, const std::string& flag)
{
    if (!flags.empty()) flags += ";";
    flags += flag;
}

bool readOneFile(
    const std::string& path,
    double expected_fps,
    double threshold_frames,
    std::vector<FrameRecord>& records,
    std::vector<FileSummary>& files,
    std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open input file: " + path;
        return false;
    }

    RawRollingFileHeader fh{};
    if (!readExact(in, &fh, static_cast<std::streamsize>(sizeof(fh)))) {
        error = "Could not read file header: " + path;
        return false;
    }
    if (fh.magic != CBRRAW_FILE_MAGIC) {
        error = "Bad file magic, not a cbrraw file: " + path;
        return false;
    }
    if (fh.version != CBRRAW_VERSION) {
        std::cerr << "[audit] warning: file " << path << " has version " << fh.version
                  << ", tool expected " << CBRRAW_VERSION << "\n";
    }

    FileSummary fs{};
    fs.path = path;
    fs.file_index = fh.file_index;
    fs.width = fh.width;
    fs.height = fh.height;
    fs.source_stride_bytes = fh.source_stride_bytes;
    fs.packed_stride_bytes = fh.packed_stride_bytes;
    fs.pixel_format = fh.pixel_format;
    fs.run_start_utc_ns = fh.run_start_utc_ns;
    fs.file_start_utc_ns = fh.file_start_utc_ns;

    const uint64_t expected_interval_ns = expected_fps > 0.0
        ? static_cast<uint64_t>(std::llround(1.0e9 / expected_fps))
        : 0ULL;
    const double suspicious_threshold_ns = static_cast<double>(expected_interval_ns) * threshold_frames;

    uint64_t file_frame_ordinal = 0;

    while (true) {
        RawRollingFrameHeader rh{};
        bool eof = false;
        std::string header_error;
        if (!readRawRollingFrameHeader(in, rh, eof, header_error)) {
            if (eof) break;
            error = "Frame header error in " + path + " near file frame " +
                    std::to_string(file_frame_ordinal) + ": " + header_error;
            return false;
        }
        if (rh.payload_bytes == 0) {
            error = "Frame with zero payload in " + path + " near file frame " + std::to_string(file_frame_ordinal);
            return false;
        }

        FrameRecord rec{};
        rec.global_row = static_cast<uint64_t>(records.size());
        rec.source_file = path;
        rec.file_index = fh.file_index;
        rec.file_frame_ordinal = file_frame_ordinal;
        rec.frame_index = rh.frame_index;
        rec.pc_utc_ns = rh.pc_utc_ns;
        rec.camera_timestamp_ns = rh.camera_timestamp_ns;
        rec.camera_frame_number = rh.camera_frame_number;

        if (!records.empty()) {
            const auto& prev = records.back();
            rec.file_rollover_boundary = (rec.file_index != prev.file_index);
            if (rec.file_rollover_boundary) appendFlag(rec.flag, "FILE_ROLLOVER_BOUNDARY");

            if (rh.pc_utc_ns >= prev.pc_utc_ns) {
                rec.delta_pc_ns = rh.pc_utc_ns - prev.pc_utc_ns;
                rec.delta_pc_ms = static_cast<double>(rec.delta_pc_ns) / 1.0e6;
            } else {
                appendFlag(rec.flag, "NONMONOTONIC_PC_TIME");
            }

            if (expected_interval_ns > 0 && rec.delta_pc_ns > 0) {
                rec.pc_delta_frames_est = static_cast<int64_t>(std::llround(
                    static_cast<double>(rec.delta_pc_ns) / static_cast<double>(expected_interval_ns)));
                if (static_cast<double>(rec.delta_pc_ns) > suspicious_threshold_ns) {
                    rec.suspected_pc_missed_triggers = std::max<int64_t>(0, rec.pc_delta_frames_est - 1);
                    appendFlag(rec.flag, "SUSPECTED_PC_GAP");
                }
            }

            if (rh.camera_timestamp_ns > 0 && prev.camera_timestamp_ns > 0) {
                if (rh.camera_timestamp_ns >= prev.camera_timestamp_ns) {
                    rec.delta_camera_ns = rh.camera_timestamp_ns - prev.camera_timestamp_ns;
                    rec.delta_camera_ms = static_cast<double>(rec.delta_camera_ns) / 1.0e6;
                    rec.camera_delta_frames_est = expected_interval_ns > 0
                        ? static_cast<int64_t>(std::llround(static_cast<double>(rec.delta_camera_ns) /
                                                            static_cast<double>(expected_interval_ns)))
                        : 0;
                    if (expected_interval_ns > 0 && static_cast<double>(rec.delta_camera_ns) > suspicious_threshold_ns) {
                        rec.suspected_camera_missed_triggers = std::max<int64_t>(0, rec.camera_delta_frames_est - 1);
                        appendFlag(rec.flag, "SUSPECTED_CAMERA_TS_GAP");
                    }
                } else {
                    appendFlag(rec.flag, "NONMONOTONIC_CAMERA_TS");
                }
            }

            if (rh.camera_frame_number > 0 && prev.camera_frame_number > 0) {
                if (rh.camera_frame_number > prev.camera_frame_number + 1) {
                    rec.camera_frame_number_gap = static_cast<int64_t>(rh.camera_frame_number - prev.camera_frame_number - 1);
                    appendFlag(rec.flag, "CAMERA_FRAME_NUMBER_GAP");
                } else if (rh.camera_frame_number <= prev.camera_frame_number) {
                    rec.camera_frame_number_gap = -1;
                    appendFlag(rec.flag, "NONMONOTONIC_CAMERA_FRAME_NUMBER");
                }
            }

            if (rh.frame_index > prev.frame_index + 1) {
                rec.frame_index_gap = static_cast<int64_t>(rh.frame_index - prev.frame_index - 1);
                appendFlag(rec.flag, "FRAME_INDEX_GAP");
            } else if (rh.frame_index <= prev.frame_index) {
                rec.frame_index_gap = -1;
                appendFlag(rec.flag, "NONMONOTONIC_FRAME_INDEX");
            }
        }

        if (fs.frames == 0) {
            fs.first_frame_index = rh.frame_index;
            fs.first_pc_utc_ns = rh.pc_utc_ns;
            fs.first_camera_timestamp_ns = rh.camera_timestamp_ns;
            fs.first_camera_frame_number = rh.camera_frame_number;
        }
        fs.last_frame_index = rh.frame_index;
        fs.last_pc_utc_ns = rh.pc_utc_ns;
        fs.last_camera_timestamp_ns = rh.camera_timestamp_ns;
        fs.last_camera_frame_number = rh.camera_frame_number;
        ++fs.frames;
        records.push_back(std::move(rec));
        ++file_frame_ordinal;

        in.seekg(static_cast<std::streamoff>(rh.payload_bytes), std::ios::cur);
        if (!in) {
            error = "Could not seek past payload in " + path + " near file frame " + std::to_string(file_frame_ordinal);
            return false;
        }
    }

    files.push_back(std::move(fs));
    return true;
}

DeltaStats computeStats(std::vector<double> values)
{
    DeltaStats s{};
    if (values.empty()) return s;

    s.count = static_cast<uint64_t>(values.size());
    double sum = 0.0;
    double sum_sq = 0.0;
    s.min_ms = std::numeric_limits<double>::infinity();
    s.max_ms = 0.0;
    for (double v : values) {
        sum += v;
        sum_sq += v * v;
        s.min_ms = std::min(s.min_ms, v);
        s.max_ms = std::max(s.max_ms, v);
    }
    s.mean_ms = sum / static_cast<double>(values.size());
    const double var = std::max(0.0, sum_sq / static_cast<double>(values.size()) - s.mean_ms * s.mean_ms);
    s.std_ms = std::sqrt(var);
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    if (n % 2 == 0) {
        s.median_ms = 0.5 * (values[n / 2 - 1] + values[n / 2]);
    } else {
        s.median_ms = values[n / 2];
    }
    return s;
}

AuditSummary summarize(
    const std::vector<FrameRecord>& records,
    const std::vector<FileSummary>& files,
    double expected_fps,
    double threshold_frames)
{
    AuditSummary s{};
    s.expected_fps = expected_fps;
    s.threshold_frames = threshold_frames;
    s.expected_interval_ns = expected_fps > 0.0
        ? static_cast<uint64_t>(std::llround(1.0e9 / expected_fps))
        : 0ULL;
    s.files_seen = static_cast<uint64_t>(files.size());
    s.total_frames = static_cast<uint64_t>(records.size());

    if (records.empty()) return s;

    s.first_pc_utc_ns = records.front().pc_utc_ns;
    s.last_pc_utc_ns = records.back().pc_utc_ns;
    s.first_frame_index = records.front().frame_index;
    s.last_frame_index = records.back().frame_index;

    std::vector<double> pc_deltas_ms;
    std::vector<double> camera_deltas_ms;
    pc_deltas_ms.reserve(records.size() > 0 ? records.size() - 1 : 0);
    camera_deltas_ms.reserve(records.size() > 0 ? records.size() - 1 : 0);

    for (const auto& r : records) {
        if (r.camera_timestamp_ns > 0) {
            ++s.camera_timestamps_seen;
            if (s.first_camera_timestamp_ns == 0) s.first_camera_timestamp_ns = r.camera_timestamp_ns;
            s.last_camera_timestamp_ns = r.camera_timestamp_ns;
        }
        if (r.camera_frame_number > 0) {
            ++s.camera_frame_numbers_seen;
        }
    }

    if (s.last_pc_utc_ns > s.first_pc_utc_ns && records.size() > 1) {
        s.duration_pc_s = static_cast<double>(s.last_pc_utc_ns - s.first_pc_utc_ns) / 1.0e9;
        s.mean_fps_by_pc_utc = static_cast<double>(records.size() - 1) / s.duration_pc_s;
    }
    if (s.last_camera_timestamp_ns > s.first_camera_timestamp_ns && records.size() > 1) {
        s.duration_camera_s = static_cast<double>(s.last_camera_timestamp_ns - s.first_camera_timestamp_ns) / 1.0e9;
        s.mean_fps_by_camera_ts = static_cast<double>(s.camera_timestamps_seen - 1) / s.duration_camera_s;
    }

    for (size_t i = 1; i < records.size(); ++i) {
        const auto& r = records[i];
        if (r.delta_pc_ns > 0) pc_deltas_ms.push_back(r.delta_pc_ms);
        if (r.delta_camera_ns > 0) {
            camera_deltas_ms.push_back(r.delta_camera_ms);
            ++s.camera_timestamp_pairs;
        }
        if (r.suspected_pc_missed_triggers > 0) {
            ++s.pc_suspicious_intervals;
            s.pc_suspected_missed_triggers += static_cast<uint64_t>(r.suspected_pc_missed_triggers);
            if (r.file_rollover_boundary) ++s.pc_suspicious_at_rollover;
        }
        if (r.suspected_camera_missed_triggers > 0) {
            ++s.camera_suspicious_intervals;
            s.camera_suspected_missed_triggers += static_cast<uint64_t>(r.suspected_camera_missed_triggers);
            if (r.file_rollover_boundary) ++s.camera_suspicious_at_rollover;
        }
        if (r.flag.find("NONMONOTONIC_CAMERA_TS") != std::string::npos) ++s.camera_timestamp_nonmonotonic;
        if (r.camera_frame_number > 0 && records[i - 1].camera_frame_number > 0) ++s.camera_frame_number_pairs;
        if (r.camera_frame_number_gap > 0) {
            ++s.camera_frame_number_gaps;
            s.total_camera_frame_number_gap += static_cast<uint64_t>(r.camera_frame_number_gap);
            if (r.file_rollover_boundary) ++s.camera_frame_number_gaps_at_rollover;
        }
        if (r.flag.find("NONMONOTONIC_CAMERA_FRAME_NUMBER") != std::string::npos) ++s.camera_frame_number_nonmonotonic;
        if (r.frame_index_gap > 0) {
            ++s.frame_index_gaps;
            s.total_frame_index_gap += static_cast<uint64_t>(r.frame_index_gap);
        }
        if (r.file_rollover_boundary) ++s.file_rollover_boundaries;
    }

    s.pc_delta = computeStats(std::move(pc_deltas_ms));
    s.camera_delta = computeStats(std::move(camera_deltas_ms));
    return s;
}

std::vector<const FrameRecord*> topPcGaps(const std::vector<FrameRecord>& records, size_t n)
{
    std::vector<const FrameRecord*> v;
    for (const auto& r : records) if (r.delta_pc_ns > 0) v.push_back(&r);
    std::sort(v.begin(), v.end(), [](const FrameRecord* a, const FrameRecord* b) {
        return a->delta_pc_ns > b->delta_pc_ns;
    });
    if (v.size() > n) v.resize(n);
    return v;
}

std::vector<const FrameRecord*> topCameraGaps(const std::vector<FrameRecord>& records, size_t n)
{
    std::vector<const FrameRecord*> v;
    for (const auto& r : records) if (r.delta_camera_ns > 0) v.push_back(&r);
    std::sort(v.begin(), v.end(), [](const FrameRecord* a, const FrameRecord* b) {
        return a->delta_camera_ns > b->delta_camera_ns;
    });
    if (v.size() > n) v.resize(n);
    return v;
}

void writeSummaryComments(std::ostream& out,
                          const AuditSummary& s,
                          const std::vector<FileSummary>& files,
                          const std::vector<FrameRecord>& records)
{
    out << "# raw_rolling_audit_version: 2\n";
    out << "# expected_fps: " << std::setprecision(12) << s.expected_fps << "\n";
    out << "# expected_interval_ns: " << s.expected_interval_ns << "\n";
    out << "# threshold_frames: " << s.threshold_frames << "\n";
    out << "# files_seen: " << s.files_seen << "\n";
    out << "# total_frames: " << s.total_frames << "\n";
    out << "# first_frame_index: " << s.first_frame_index << "\n";
    out << "# last_frame_index: " << s.last_frame_index << "\n";

    out << "# first_pc_utc_ns: " << s.first_pc_utc_ns << "\n";
    out << "# first_pc_utc_iso: " << utcNsToIso(s.first_pc_utc_ns) << "\n";
    out << "# last_pc_utc_ns: " << s.last_pc_utc_ns << "\n";
    out << "# last_pc_utc_iso: " << utcNsToIso(s.last_pc_utc_ns) << "\n";
    out << "# duration_pc_s: " << s.duration_pc_s << "\n";
    out << "# mean_fps_by_pc_utc: " << s.mean_fps_by_pc_utc << "\n";
    out << "# pc_delta_count: " << s.pc_delta.count << "\n";
    out << "# pc_mean_delta_ms: " << s.pc_delta.mean_ms << "\n";
    out << "# pc_median_delta_ms: " << s.pc_delta.median_ms << "\n";
    out << "# pc_min_delta_ms: " << s.pc_delta.min_ms << "\n";
    out << "# pc_max_delta_ms: " << s.pc_delta.max_ms << "\n";
    out << "# pc_std_delta_ms: " << s.pc_delta.std_ms << "\n";
    out << "# pc_suspicious_intervals: " << s.pc_suspicious_intervals << "\n";
    out << "# pc_suspected_missed_triggers: " << s.pc_suspected_missed_triggers << "\n";

    out << "# camera_timestamps_seen: " << s.camera_timestamps_seen << "\n";
    out << "# camera_timestamp_pairs: " << s.camera_timestamp_pairs << "\n";
    out << "# first_camera_timestamp_ns: " << s.first_camera_timestamp_ns << "\n";
    out << "# last_camera_timestamp_ns: " << s.last_camera_timestamp_ns << "\n";
    out << "# duration_camera_s: " << s.duration_camera_s << "\n";
    out << "# mean_fps_by_camera_ts: " << s.mean_fps_by_camera_ts << "\n";
    out << "# camera_mean_delta_ms: " << s.camera_delta.mean_ms << "\n";
    out << "# camera_median_delta_ms: " << s.camera_delta.median_ms << "\n";
    out << "# camera_min_delta_ms: " << s.camera_delta.min_ms << "\n";
    out << "# camera_max_delta_ms: " << s.camera_delta.max_ms << "\n";
    out << "# camera_std_delta_ms: " << s.camera_delta.std_ms << "\n";
    out << "# camera_suspicious_intervals: " << s.camera_suspicious_intervals << "\n";
    out << "# camera_suspected_missed_triggers: " << s.camera_suspected_missed_triggers << "\n";
    out << "# camera_timestamp_nonmonotonic: " << s.camera_timestamp_nonmonotonic << "\n";

    out << "# camera_frame_numbers_seen: " << s.camera_frame_numbers_seen << "\n";
    out << "# camera_frame_number_pairs: " << s.camera_frame_number_pairs << "\n";
    out << "# camera_frame_number_gaps: " << s.camera_frame_number_gaps << "\n";
    out << "# total_camera_frame_number_gap: " << s.total_camera_frame_number_gap << "\n";
    out << "# camera_frame_number_nonmonotonic: " << s.camera_frame_number_nonmonotonic << "\n";
    out << "# camera_frame_number_gaps_at_rollover: " << s.camera_frame_number_gaps_at_rollover << "\n";

    out << "# frame_index_gaps: " << s.frame_index_gaps << "\n";
    out << "# total_frame_index_gap: " << s.total_frame_index_gap << "\n";
    out << "# file_rollover_boundaries: " << s.file_rollover_boundaries << "\n";
    out << "# pc_suspicious_at_rollover: " << s.pc_suspicious_at_rollover << "\n";
    out << "# camera_suspicious_at_rollover: " << s.camera_suspicious_at_rollover << "\n";

    for (const auto& f : files) {
        out << "# file: " << f.path
            << " file_index=" << f.file_index
            << " frames=" << f.frames
            << " width=" << f.width
            << " height=" << f.height
            << " pixel_format=" << pixelFormatName(f.pixel_format)
            << " first_frame=" << f.first_frame_index
            << " last_frame=" << f.last_frame_index
            << " first_camera_ts_ns=" << f.first_camera_timestamp_ns
            << " last_camera_ts_ns=" << f.last_camera_timestamp_ns
            << " first_camera_frame_number=" << f.first_camera_frame_number
            << " last_camera_frame_number=" << f.last_camera_frame_number
            << "\n";
    }

    const auto pc_top = topPcGaps(records, 20);
    for (size_t i = 0; i < pc_top.size(); ++i) {
        const auto* r = pc_top[i];
        out << "# top_pc_gap_" << (i + 1)
            << ": frame_index=" << r->frame_index
            << " file_index=" << r->file_index
            << " file_frame=" << r->file_frame_ordinal
            << " delta_ms=" << r->delta_pc_ms
            << " rollover=" << (r->file_rollover_boundary ? "true" : "false")
            << " flags=" << r->flag
            << "\n";
    }

    const auto cam_top = topCameraGaps(records, 20);
    for (size_t i = 0; i < cam_top.size(); ++i) {
        const auto* r = cam_top[i];
        out << "# top_camera_gap_" << (i + 1)
            << ": frame_index=" << r->frame_index
            << " file_index=" << r->file_index
            << " file_frame=" << r->file_frame_ordinal
            << " delta_ms=" << r->delta_camera_ms
            << " rollover=" << (r->file_rollover_boundary ? "true" : "false")
            << " flags=" << r->flag
            << "\n";
    }
}

void writeCsv(
    const std::string& out_path,
    const std::vector<FrameRecord>& records,
    const std::vector<FileSummary>& files,
    const AuditSummary& summary)
{
    std::ofstream out(out_path);
    if (!out) {
        throw std::runtime_error("Could not open CSV for writing: " + out_path);
    }

    writeSummaryComments(out, summary, files, records);
    out << "global_row,source_file,file_index,file_frame_ordinal,frame_index,file_rollover_boundary,"
           "pc_utc_ns,pc_utc_iso,camera_timestamp_ns,camera_frame_number,"
           "delta_pc_ns,delta_pc_ms,pc_delta_frames_est,suspected_pc_missed_triggers,"
           "delta_camera_ns,delta_camera_ms,camera_delta_frames_est,suspected_camera_missed_triggers,"
           "expected_interval_ns,frame_index_gap,camera_frame_number_gap,flag\n";

    out << std::setprecision(12);
    for (const auto& r : records) {
        out << r.global_row << ','
            << '"' << r.source_file << '"' << ','
            << r.file_index << ','
            << r.file_frame_ordinal << ','
            << r.frame_index << ','
            << (r.file_rollover_boundary ? 1 : 0) << ','
            << r.pc_utc_ns << ','
            << utcNsToIso(r.pc_utc_ns) << ','
            << r.camera_timestamp_ns << ','
            << r.camera_frame_number << ','
            << r.delta_pc_ns << ','
            << r.delta_pc_ms << ','
            << r.pc_delta_frames_est << ','
            << r.suspected_pc_missed_triggers << ','
            << r.delta_camera_ns << ','
            << r.delta_camera_ms << ','
            << r.camera_delta_frames_est << ','
            << r.suspected_camera_missed_triggers << ','
            << summary.expected_interval_ns << ','
            << r.frame_index_gap << ','
            << r.camera_frame_number_gap << ','
            << '"' << r.flag << '"'
            << '\n';
    }
}

void printUsage()
{
    std::cerr
        << "Usage:\n"
        << "  raw_rolling_audit <first_0000.cbrraw> <out.csv> <expected_fps> [threshold_frames=1.5] [max_files=0]\n\n"
        << "Examples:\n"
        << "  raw_rolling_audit /tmp/run_0000.cbrraw /tmp/run_timestamps.csv 100\n"
        << "  raw_rolling_audit /tmp/run_0000.cbrraw /tmp/run_timestamps.csv 100 1.5 10\n\n"
        << "Notes:\n"
        << "  If the first input ends in _0000.cbrraw, this tool automatically scans _0001, _0002, ...\n"
        << "  The CSV starts with comment lines containing summary statistics. Use pandas.read_csv(path, comment='#').\n"
        << "  PC timestamp deltas measure when cbrng received frames. Camera timestamp deltas use XIMEA XI_IMG tsSec/tsUSec when present.\n"
        << "  Camera frame number gaps use XIMEA XI_IMG.nframe when present and are the strongest skip indicator.\n";
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        printUsage();
        return 2;
    }

    const std::string first_path = argv[1];
    const std::string out_csv = argv[2];
    const double expected_fps = std::stod(argv[3]);
    const double threshold_frames = argc >= 5 ? std::stod(argv[4]) : 1.5;
    const int max_files = argc >= 6 ? std::stoi(argv[5]) : 0;

    if (expected_fps <= 0.0) {
        std::cerr << "expected_fps must be > 0\n";
        return 2;
    }
    if (threshold_frames <= 1.0) {
        std::cerr << "threshold_frames should be > 1.0; common value is 1.5\n";
        return 2;
    }

    std::vector<FrameRecord> records;
    std::vector<FileSummary> files;

    int index = 0;
    while (true) {
        if (max_files > 0 && index >= max_files) break;
        const std::string path = makeIndexedPath(first_path, index);
        if (path.empty()) break;
        if (!fileExists(path)) {
            if (index == 0) {
                std::cerr << "Input file does not exist: " << path << "\n";
                return 1;
            }
            break;
        }

        std::cout << "[audit] reading " << path << "\n";
        std::string error;
        if (!readOneFile(path, expected_fps, threshold_frames, records, files, error)) {
            std::cerr << "[audit] error: " << error << "\n";
            return 1;
        }
        ++index;
    }

    const AuditSummary summary = summarize(records, files, expected_fps, threshold_frames);

    std::cout << "\n";
    writeSummaryComments(std::cout, summary, files, records);
    std::cout << "\n[audit] writing CSV: " << out_csv << "\n";

    try {
        writeCsv(out_csv, records, files, summary);
    } catch (const std::exception& e) {
        std::cerr << "[audit] error: " << e.what() << "\n";
        return 1;
    }

    if (summary.pc_suspicious_intervals > 0 ||
        summary.camera_suspicious_intervals > 0 ||
        summary.frame_index_gaps > 0 ||
        summary.camera_timestamp_nonmonotonic > 0 ||
        summary.camera_frame_number_gaps > 0 ||
        summary.camera_frame_number_nonmonotonic > 0) {
        std::cout << "[audit] FLAGS FOUND: pc_suspicious_intervals=" << summary.pc_suspicious_intervals
                  << ", pc_suspected_missed_triggers=" << summary.pc_suspected_missed_triggers
                  << ", camera_suspicious_intervals=" << summary.camera_suspicious_intervals
                  << ", camera_suspected_missed_triggers=" << summary.camera_suspected_missed_triggers
                  << ", frame_index_gaps=" << summary.frame_index_gaps
                  << ", total_frame_index_gap=" << summary.total_frame_index_gap
                  << ", pc_suspicious_at_rollover=" << summary.pc_suspicious_at_rollover
                  << ", camera_suspicious_at_rollover=" << summary.camera_suspicious_at_rollover
                  << ", camera_frame_number_gaps=" << summary.camera_frame_number_gaps
                  << ", total_camera_frame_number_gap=" << summary.total_camera_frame_number_gap
                  << ", camera_frame_number_gaps_at_rollover=" << summary.camera_frame_number_gaps_at_rollover
                  << "\n";
        return 3;
    }

    std::cout << "[audit] OK: no suspicious PC/camera intervals or frame index gaps detected.\n";
    return 0;
}
