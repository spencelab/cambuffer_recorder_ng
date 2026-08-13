#include "cambuffer_recorder_ng/FfmpegWriter.hpp"
#include "cambuffer_recorder_ng/raw/RawRollingFormat.hpp"
#include "cambuffer_recorder_ng/raw/Crc32.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace cambuffer_recorder_ng;

namespace
{

volatile std::sig_atomic_t g_stop_requested = 0;

void handleSignal(int signum)
{
    g_stop_requested = signum;
}

bool stopRequested()
{
    return g_stop_requested != 0;
}

struct WhiteBalanceGains
{
    bool enabled = false;
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
};

std::string trim(std::string s)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string unquote(std::string s)
{
    s = trim(std::move(s));
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

bool fileExists(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

std::string inferMetadataPath(const std::string& input_path)
{
    // /tmp/prefix_0000.cbrraw -> /tmp/prefix.metadata.yaml
    static const std::regex indexed_file_re(R"(^(.*)_([0-9]{4})\.cbrraw$)");
    std::smatch m;
    if (std::regex_match(input_path, m, indexed_file_re)) {
        return m[1].str() + ".metadata.yaml";
    }

    // Fallback: /tmp/name.cbrraw -> /tmp/name.metadata.yaml
    static const std::regex plain_file_re(R"(^(.*)\.cbrraw$)");
    if (std::regex_match(input_path, m, plain_file_re)) {
        return m[1].str() + ".metadata.yaml";
    }

    return input_path + ".metadata.yaml";
}

bool splitRollingPrefix(const std::string& input_path, std::string& prefix, uint32_t& first_index)
{
    static const std::regex indexed_file_re(R"(^(.*)_([0-9]{4})\.cbrraw$)");
    std::smatch m;
    if (!std::regex_match(input_path, m, indexed_file_re)) return false;
    prefix = m[1].str();
    first_index = static_cast<uint32_t>(std::stoul(m[2].str()));
    return true;
}

std::string rollingFileName(const std::string& prefix, uint32_t index)
{
    std::ostringstream oss;
    oss << prefix << "_" << std::setw(4) << std::setfill('0') << index << ".cbrraw";
    return oss.str();
}

std::string metadataValue(const std::string& metadata_text, const std::string& key)
{
    // Metadata currently writes settings as flat YAML keys, e.g.
    //     pipeline.white_balance.r_gain: 1.23
    // We intentionally take the last occurrence so effective_settings wins over requested_settings.
    std::istringstream in(metadata_text);
    std::string line;
    std::string found;
    const std::string needle = key + ":";

    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const auto pos = line.find(needle);
        if (pos == std::string::npos) continue;
        found = trim(line.substr(pos + needle.size()));
    }

    return unquote(found);
}

std::string readFileToString(const std::string& path)
{
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool parseBool(const std::string& s, bool fallback)
{
    std::string x = s;
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    x = trim(x);
    if (x == "true" || x == "yes" || x == "1" || x == "on") return true;
    if (x == "false" || x == "no" || x == "0" || x == "off") return false;
    return fallback;
}

double parseDouble(const std::string& s, double fallback)
{
    if (s.empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str()) return fallback;
    return v;
}

WhiteBalanceGains readWhiteBalanceFromMetadata(const std::string& metadata_path)
{
    WhiteBalanceGains wb;
    const std::string text = readFileToString(metadata_path);
    if (text.empty()) return wb;

    wb.enabled = parseBool(metadataValue(text, "pipeline.white_balance.enabled"), false);
    wb.r = parseDouble(metadataValue(text, "pipeline.white_balance.r_gain"), 1.0);
    wb.g = parseDouble(metadataValue(text, "pipeline.white_balance.g_gain"), 1.0);
    wb.b = parseDouble(metadataValue(text, "pipeline.white_balance.b_gain"), 1.0);

    // If the user supplied non-unity gains but forgot enabled=true, do the helpful thing.
    if (std::fabs(wb.r - 1.0) > 1e-9 ||
        std::fabs(wb.g - 1.0) > 1e-9 ||
        std::fabs(wb.b - 1.0) > 1e-9) {
        wb.enabled = true;
    }

    return wb;
}

int readFpsFromMetadata(const std::string& metadata_path, int fallback)
{
    const std::string text = readFileToString(metadata_path);
    if (text.empty()) return fallback;
    const double fps = parseDouble(metadataValue(text, "camera.fps"), static_cast<double>(fallback));
    if (fps <= 0.0) return fallback;
    return std::max(1, static_cast<int>(std::lround(fps)));
}


const char* rawPixelFormatToString(uint32_t fmt)
{
    switch (static_cast<RawPixelFormat>(fmt)) {
        case RawPixelFormat::RAW8_BAYER_GBRG: return "RAW8_BAYER_GBRG";
        case RawPixelFormat::RAW8_MONO: return "RAW8_MONO";
        default: return "UNKNOWN";
    }
}

bool isSupportedRawPixelFormat(uint32_t fmt)
{
    return fmt == static_cast<uint32_t>(RawPixelFormat::RAW8_BAYER_GBRG) ||
           fmt == static_cast<uint32_t>(RawPixelFormat::RAW8_MONO);
}

uint8_t clampU8(double v)
{
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint8_t>(std::lround(v));
}

void applyWhiteBalanceRgb24(cv::Mat& rgb, const WhiteBalanceGains& wb)
{
    if (!wb.enabled) return;
    if (rgb.empty() || rgb.type() != CV_8UC3) return;

    for (int y = 0; y < rgb.rows; ++y) {
        auto* row = rgb.ptr<uint8_t>(y);
        for (int x = 0; x < rgb.cols; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 3;
            // FfmpegWriter expects RGB24, and cvtColor below requests RGB output.
            px[0] = clampU8(static_cast<double>(px[0]) * wb.r);
            px[1] = clampU8(static_cast<double>(px[1]) * wb.g);
            px[2] = clampU8(static_cast<double>(px[2]) * wb.b);
        }
    }
}

void applyGammaRgb24(cv::Mat& rgb, double gamma)
{
    if (rgb.empty() || rgb.type() != CV_8UC3) return;
    if (!(gamma > 0.0) || std::fabs(gamma - 1.0) < 1e-9) return;

    const double inv_gamma = 1.0 / gamma;
    for (int y = 0; y < rgb.rows; ++y) {
        auto* row = rgb.ptr<uint8_t>(y);
        for (int x = 0; x < rgb.cols; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 3;
            for (int c = 0; c < 3; ++c) {
                const double normalized = static_cast<double>(px[c]) / 255.0;
                px[c] = clampU8(std::pow(normalized, inv_gamma) * 255.0);
            }
        }
    }
}

bool endsWithCaseInsensitive(const std::string& text, const std::string& suffix)
{
    if (suffix.size() > text.size()) return false;
    const auto offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const auto a = static_cast<unsigned char>(text[offset + i]);
        const auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

void printUsage()
{
    std::cerr
        << "Usage: raw_rolling_to_mp4 <input_0000.cbrraw> <output.mp4|output.png> [max_frames] [fps] [metadata.yaml] [r_gain g_gain b_gain [gamma]]\n"
        << "\n"
        << "  max_frames=0 means convert all contiguous rolling files for MP4 output.\n"
        << "  If output ends in .png, the first decoded frame is written as a PNG thumbnail.\n"
        << "  If metadata.yaml is omitted, it is inferred from the input name.\n"
        << "  White balance is read from metadata keys:\n"
        << "    pipeline.white_balance.enabled\n"
        << "    pipeline.white_balance.r_gain\n"
        << "    pipeline.white_balance.g_gain\n"
        << "    pipeline.white_balance.b_gain\n"
        << "  Optional gamma applies after debayer/white-balance. gamma=1 leaves pixels unchanged.\n";
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        printUsage();
        return 2;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const std::string input = argv[1];
    const std::string output = argv[2];
    const bool output_is_png = endsWithCaseInsensitive(output, ".png");
    const uint64_t max_frames = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 0;

    std::string metadata_path = (argc > 5) ? std::string(argv[5]) : inferMetadataPath(input);
    if (metadata_path == "none" || metadata_path == "-") metadata_path.clear();

    int fps = 30;
    if (!metadata_path.empty()) fps = readFpsFromMetadata(metadata_path, fps);
    if (argc > 4) {
        const int cli_fps = std::atoi(argv[4]);
        if (cli_fps > 0) fps = cli_fps;
    }

    WhiteBalanceGains wb;
    if (!metadata_path.empty()) wb = readWhiteBalanceFromMetadata(metadata_path);
    if (argc > 8) {
        wb.enabled = true;
        wb.r = std::strtod(argv[6], nullptr);
        wb.g = std::strtod(argv[7], nullptr);
        wb.b = std::strtod(argv[8], nullptr);
    }
    double gamma = 1.0;
    if (argc > 9) {
        gamma = std::strtod(argv[9], nullptr);
        if (!(gamma > 0.0)) gamma = 1.0;
    }

    if (!metadata_path.empty()) {
        if (fileExists(metadata_path)) {
            std::cerr << "[raw2mp4] metadata: " << metadata_path << "\n";
        } else {
            std::cerr << "[raw2mp4] metadata not found: " << metadata_path
                      << " (continuing with defaults)\n";
        }
    }
    constexpr int kH264Crf = 24;
    std::cerr << "[raw2mp4] output fps: " << fps << "\n";
    if (!output_is_png) {
        std::cerr << "[raw2mp4] encoder: libx264, yuv420p, CRF " << kH264Crf
                  << ", preset medium (constant-quality mode)\n";
    }
    std::cerr << "[raw2mp4] white balance: "
              << (wb.enabled ? "enabled" : "disabled")
              << " R=" << wb.r << " G=" << wb.g << " B=" << wb.b
              << "; gamma=" << gamma << "\n";
    if (output_is_png) {
        std::cerr << "[raw2mp4] PNG thumbnail mode: writing first decoded frame to " << output << "\n";
    }

    std::string rolling_prefix;
    uint32_t first_index = 0;
    const bool has_indexed_name = splitRollingPrefix(input, rolling_prefix, first_index);

    FfmpegWriter writer;
    bool writer_open = false;
    uint64_t written = 0;
    uint32_t file_index = first_index;

    while (!stopRequested() && (max_frames == 0 || written < max_frames)) {
        const std::string current_input = has_indexed_name
            ? rollingFileName(rolling_prefix, file_index)
            : input;

        if (!fileExists(current_input)) {
            if (file_index == first_index) {
                std::cerr << "[raw2mp4] input file not found: " << current_input << "\n";
                return 1;
            }
            std::cerr << "[raw2mp4] no next rolling file at " << current_input
                      << "; conversion complete\n";
            break;
        }

        std::cerr << "[raw2mp4] opening input file " << current_input << "\n";
        FILE* fp = fopen(current_input.c_str(), "rb");
        if (!fp) { perror("fopen"); return 1; }

        RawRollingFileHeader fh{};
        if (fread(&fh, 1, sizeof(fh), fp) != sizeof(fh) || fh.magic != CBRRAW_FILE_MAGIC) {
            std::cerr << "Bad or missing cbrraw file header in " << current_input << "\n";
            fclose(fp);
            return 1;
        }

        if (!isSupportedRawPixelFormat(fh.pixel_format)) {
            std::cerr << "Unsupported cbrraw pixel format in file header: "
                      << fh.pixel_format << " (" << rawPixelFormatToString(fh.pixel_format)
                      << ") in " << current_input << "\n";
            fclose(fp);
            return 1;
        }

        const bool file_is_mono =
            fh.pixel_format == static_cast<uint32_t>(RawPixelFormat::RAW8_MONO);
        const bool file_is_bayer =
            fh.pixel_format == static_cast<uint32_t>(RawPixelFormat::RAW8_BAYER_GBRG);

        if (!output_is_png && !writer_open) {
            if (!writer.open(output, static_cast<int>(fh.width), static_cast<int>(fh.height),
                             fps, "libx264", kH264Crf)) {
                std::cerr << "Could not open output movie " << output << "\n";
                fclose(fp);
                return 1;
            }
            writer_open = true;
            std::cerr << "[raw2mp4] writing " << output << " at "
                      << fh.width << "x" << fh.height
                      << ", input pixel_format=" << rawPixelFormatToString(fh.pixel_format)
                      << (file_is_mono ? " (mono grayscale -> RGB MP4)" : " (Bayer GBRG -> RGB MP4)")
                      << "\n";
            if (file_is_mono && wb.enabled) {
                std::cerr << "[raw2mp4] input is RAW8_MONO; ignoring white-balance gains for grayscale conversion\n";
            }
        }

        std::vector<uint8_t> raw(static_cast<size_t>(fh.width) * static_cast<size_t>(fh.height));
        cv::Mat raw_mat(static_cast<int>(fh.height), static_cast<int>(fh.width), CV_8UC1, raw.data());
        cv::Mat rgb;

        uint64_t frames_this_file = 0;
        while (!stopRequested() && (max_frames == 0 || written < max_frames)) {
            RawRollingFrameHeader rh{};
            bool eof = false;
            std::string header_error;
            if (!readRawRollingFrameHeader(fp, rh, eof, header_error)) {
                if (eof) break;
                std::cerr << "Bad frame header after " << written << " frames in " << current_input
                          << ": " << header_error << "\n";
                fclose(fp);
                writer.close();
                return 1;
            }
            if (rh.payload_bytes != raw.size()) {
                std::cerr << "Unexpected payload size " << rh.payload_bytes
                          << ", expected " << raw.size() << " in " << current_input << "\n";
                fclose(fp);
                writer.close();
                return 1;
            }
            if (fread(raw.data(), 1, raw.size(), fp) != raw.size()) {
                std::cerr << "Short payload after " << written << " frames in " << current_input << "\n";
                fclose(fp);
                writer.close();
                return 1;
            }
            if (rh.header_flags & CBRRAW_FRAME_FLAG_PAYLOAD_CRC32) {
                const uint32_t actual_crc = crc32Ieee(raw.data(), raw.size());
                if (actual_crc != rh.payload_crc32) {
                    std::cerr << "Payload CRC32 mismatch after " << written << " frames in "
                              << current_input << ": expected " << rh.payload_crc32
                              << ", got " << actual_crc << "\n";
                    fclose(fp);
                    writer.close();
                    return 1;
                }
            }

            if (rh.pixel_format != fh.pixel_format) {
                std::cerr << "Frame pixel format changed within rolling file at frame_index "
                          << rh.frame_index << ": header=" << rawPixelFormatToString(fh.pixel_format)
                          << " frame=" << rawPixelFormatToString(rh.pixel_format)
                          << " (" << rh.pixel_format << ")\n";
                fclose(fp);
                writer.close();
                return 1;
            }

            if (file_is_bayer) {
                // XIMEA/FFmpeg bayer_gbrg8 is row0: G B, row1: R G.
                // OpenCV's Bayer conversion names are easy to misread here:
                // COLOR_BayerGB2RGB produces red/blue swapped output for this
                // camera's GBRG stream.  COLOR_BayerGR2RGB is the OpenCV code
                // that yields packed RGB bytes matching FFmpegWriter's RGB24
                // input expectation.  (It is an alias of COLOR_BayerGB2BGR.)
                cv::cvtColor(raw_mat, rgb, cv::COLOR_BayerGR2RGB);
                applyWhiteBalanceRgb24(rgb, wb);
            } else if (file_is_mono) {
                // Monochrome RAW8 has no Bayer pattern and no color channels.
                // FfmpegWriter currently accepts RGB24, so duplicate gray into RGB.
                cv::cvtColor(raw_mat, rgb, cv::COLOR_GRAY2RGB);
            } else {
                std::cerr << "Unsupported frame pixel format " << rh.pixel_format
                          << " at frame_index " << rh.frame_index << "\n";
                fclose(fp);
                writer.close();
                return 1;
            }

            applyGammaRgb24(rgb, gamma);

            if (output_is_png) {
                cv::Mat bgr;
                cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
                if (!cv::imwrite(output, bgr)) {
                    std::cerr << "Could not write PNG thumbnail " << output << "\n";
                    fclose(fp);
                    return 1;
                }
                std::cerr << "[raw2mp4] wrote PNG thumbnail from frame_index " << rh.frame_index
                          << " to " << output << "\n";
                fclose(fp);
                return 0;
            }

            writer.write_frame(rgb.data, static_cast<int>(rgb.step));
            ++written;
            ++frames_this_file;

            if (written % 200 == 0) {
                std::cerr << "[raw2mp4] processed " << written << " frames"
                          << " (current file " << current_input
                          << ", last frame_index " << rh.frame_index
                          << ", pc_utc_ns " << rh.pc_utc_ns << ")\n";
            }
        }

        if (stopRequested()) {
            std::cerr << "[raw2mp4] stop requested by signal " << g_stop_requested
                      << "; closing current input file and finalizing MP4...\n";
        }

        fclose(fp);
        std::cerr << "[raw2mp4] finished " << current_input
                  << " after " << frames_this_file << " frames from this file\n";

        if (stopRequested()) break;
        if (!has_indexed_name) break;
        ++file_index;
    }

    if (writer_open) {
        std::cerr << "[raw2mp4] finalizing MP4 writer...\n";
        writer.close();
    }

    if (stopRequested()) {
        std::cerr << "[raw2mp4] interrupted cleanly after writing " << written
                  << " frames to " << output << "\n";
        return 130;
    }

    std::cerr << "[raw2mp4] wrote " << written << " frames to " << output << "\n";
    return 0;
}
