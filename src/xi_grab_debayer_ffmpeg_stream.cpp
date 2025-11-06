#include <iostream>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <csignal>
#include <algorithm>  // for std::min
#include <m3api/xiApi.h>

#ifndef XI_PRM_PADDING_X
#define XI_PRM_PADDING_X "padding_x"
#endif

using namespace std::chrono;

// ===============================
//   Bayer pattern + DebayerHalf
// ===============================
enum class BayerPattern { RGGB, GRBG, GBRG, BGGR };

inline void debayer_half_color(const uint8_t* src, int src_w, int src_h, int src_stride,
                               uint8_t* dst, BayerPattern pattern)
{
    const int dst_w = src_w / 2;
    const int dst_h = src_h / 2;

    for (int y = 0; y < dst_h; ++y) {
        const uint8_t* row0 = src + (2 * y) * src_stride;
        const uint8_t* row1 = row0 + src_stride;
        uint8_t* out = dst + y * dst_w * 3;

        for (int x = 0; x < dst_w; ++x) {
            uint8_t r, g, b;
            switch (pattern) {
            case BayerPattern::RGGB:
                r = row0[2*x];
                g = (row0[2*x+1] + row1[2*x]) / 2;
                b = row1[2*x+1];
                break;
            case BayerPattern::GRBG:
                r = row0[2*x+1];
                g = (row0[2*x] + row1[2*x+1]) / 2;
                b = row1[2*x];
                break;
            case BayerPattern::GBRG:
                r = row1[2*x];
                g = (row0[2*x] + row1[2*x+1]) / 2;
                b = row0[2*x+1];
                break;
            case BayerPattern::BGGR:
                r = row1[2*x+1];
                g = (row0[2*x+1] + row1[2*x]) / 2;
                b = row0[2*x];
                break;
            }
            out[3*x + 0] = b;  // output in BGR order for ffmpeg
            out[3*x + 1] = g;
            out[3*x + 2] = r;
        }
    }
}

#include <algorithm>  // for std::min

// Additive-binning half debayer with color output.
// Accepts RAW8 Bayer buffer and outputs RGB24 (BGR order for FFmpeg).
void debayer_half_additive_color(
    const uint8_t* src,
    int w, int h, int stride,
    uint8_t* dst,
    BayerPattern pattern)
{
    int out_w = w / 2;
    int out_h = h / 2;

    for (int y = 0; y < out_h; ++y) {
        const uint8_t* row0 = src + (2 * y) * stride;
        const uint8_t* row1 = src + (2 * y + 1) * stride;
        uint8_t* out = dst + y * out_w * 3;

        for (int x = 0; x < out_w; ++x) {
            int x2 = x * 2;

            uint8_t r = 0, g = 0, b = 0;
            switch (pattern) {
                case BayerPattern::GBRG: {
                    uint8_t G1 = row0[x2];
                    uint8_t R  = row0[x2 + 1];
                    uint8_t B  = row1[x2];
                    uint8_t G2 = row1[x2 + 1];
                    g = (G1 + G2) >> 1;  // average both greens
                    r = R;
                    b = B;
                    break;
                }
                case BayerPattern::GRBG: {
                    uint8_t G1 = row0[x2 + 1];
                    uint8_t R  = row0[x2];
                    uint8_t B  = row1[x2 + 1];
                    uint8_t G2 = row1[x2];
                    g = (G1 + G2) >> 1;
                    r = R;
                    b = B;
                    break;
                }
                case BayerPattern::RGGB: {
                    uint8_t R1 = row0[x2];
                    uint8_t G1 = row0[x2 + 1];
                    uint8_t G2 = row1[x2];
                    uint8_t B1 = row1[x2 + 1];
                    r = R1;
                    g = (G1 + G2) >> 1;
                    b = B1;
                    break;
                }
                case BayerPattern::BGGR: {
                    uint8_t B1 = row0[x2];
                    uint8_t G1 = row0[x2 + 1];
                    uint8_t G2 = row1[x2];
                    uint8_t R1 = row1[x2 + 1];
                    r = R1;
                    g = (G1 + G2) >> 1;
                    b = B1;
                    break;
                }
            }

            // For additive binning instead of averaging brightness:
            // g = std::min(G1 + G2, 255);

            out[3 * x + 0] = b;
            out[3 * x + 1] = g;
            out[3 * x + 2] = r;
        }
    }
}



// ===============================
//   Print camera parameters
// ===============================
static void print_cam_cfg(HANDLE handle)
{
    int cam_w=0, cam_h=0, exp=0, padx=0, fmt=0;
    xiGetParamInt(handle, XI_PRM_WIDTH, &cam_w);
    xiGetParamInt(handle, XI_PRM_HEIGHT, &cam_h);
    xiGetParamInt(handle, XI_PRM_EXPOSURE, &exp);
    xiGetParamInt(handle, XI_PRM_IMAGE_DATA_FORMAT, &fmt);
    xiGetParamInt(handle, XI_PRM_PADDING_X, &padx);

    std::cout << "XI config: width=" << cam_w
              << " height=" << cam_h
              << " exposure(us)=" << exp
              << " padding_x=" << padx
              << " data_format=" << fmt
              << " (expect RAW8=" << XI_RAW8 << ")\n";
}

// ===============================
//   Main benchmark
// ===============================
int main(int argc, char** argv)
{
    int num_frames = 300;
    BayerPattern pattern = BayerPattern::GBRG;  // GBRG for your camera
    const char* outfile = "xi_stream_test.mp4";

    if (argc > 1) num_frames = std::atoi(argv[1]);
    std::cout << "Capturing " << num_frames << " frames using pattern GBRG\n";

    std::signal(SIGPIPE, SIG_IGN);

    HANDLE cam = nullptr;
    XI_RETURN stat = xiOpenDevice(0, &cam);
    if (stat != XI_OK) { std::cerr << "xiOpenDevice failed\n"; return 1; }

    int width = 2048, height = 704;
    xiSetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(cam, XI_PRM_WIDTH, width);
    xiSetParamInt(cam, XI_PRM_HEIGHT, height);
    xiSetParamInt(cam, XI_PRM_EXPOSURE, 2000);
    // could do gain...
    //xiSetParamInt(cam, XI_PRM_BUFFERS_QUEUE_SIZE, 2);

    print_cam_cfg(cam);

    XI_IMG img = {};
    img.size = sizeof(XI_IMG);

    int actual_w=0, actual_h=0;
    xiGetParamInt(cam, XI_PRM_WIDTH, &actual_w);
    xiGetParamInt(cam, XI_PRM_HEIGHT, &actual_h);
    actual_w &= ~1; actual_h &= ~1;
    int out_w = actual_w / 2, out_h = actual_h / 2;
    size_t rgb_bytes = static_cast<size_t>(out_w) * out_h * 3;

    std::cout << "==== Camera ROI and Debayer Config ====\n";
    std::cout << "Sensor ROI: " << actual_w << " x " << actual_h << "\n";
    std::cout << "DebayerHalf Output: " << out_w << " x " << out_h
              << " (" << rgb_bytes << " bytes per RGB frame)\n";
    std::cout << "=======================================\n";

    std::vector<uint8_t> debayer_buf(rgb_bytes);

    char cmd[512];
    /* blurry, fast, small files and 160fps */
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -f rawvideo -pix_fmt bgr24 -s %dx%d -r 100 "
             "-i pipe:0 -y -threads 0 -preset ultrafast "
             "-c:v libx264 -crf 18 -pix_fmt yuv420p %s",
             out_w, out_h, outfile);
    
    // super slow huge files!
    //-preset fast -crf 0
    /*
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -f rawvideo -pix_fmt bgr24 -s %dx%d -r 100 "
             "-i pipe:0 -y -threads 0 -preset fast "
             "-c:v libx264 -crf 0 -pix_fmt yuv420p %s",
             out_w, out_h, outfile);    */
    FILE* ffmpeg = popen(cmd, "w");
    if (!ffmpeg) { perror("popen"); xiCloseDevice(cam); return 1; }

    xiStartAcquisition(cam);

    int padx = 0;
    if (xiGetParamInt(cam, XI_PRM_PADDING_X, &padx) != XI_OK)
        padx = 0;  // fallback silently

    const int stride = width + padx;

    double t_grab=0, t_proc=0, t_enc=0;
    for (int i=0; i<num_frames; ++i) {
        auto t0 = high_resolution_clock::now();
        stat = xiGetImage(cam, 100, &img);
        if (stat != XI_OK || !img.bp) { std::cerr << "xiGetImage failed\n"; break; }
        auto t1 = high_resolution_clock::now();

        debayer_half_color(static_cast<uint8_t*>(img.bp),
                           img.width, img.height, stride,
                           debayer_buf.data(), pattern);
        //debayer_half_additive_color(src, w, h, stride, dst, BayerPattern::GBRG);
        debayer_half_additive_color(static_cast<uint8_t*>(img.bp),
                           img.width, img.height, stride,
                           debayer_buf.data(), pattern);

        auto t2 = high_resolution_clock::now();

        //std::cout << "Writing " << rgb_bytes
        //  << " bytes (" << out_w << "x" << out_h << ")\n";

        size_t written = fwrite(debayer_buf.data(), 1, rgb_bytes, ffmpeg);
        if (written != rgb_bytes) { std::cerr << "pipe write failed\n"; break; }
        auto t3 = high_resolution_clock::now();

        t_grab += duration<double, std::milli>(t1 - t0).count();
        t_proc += duration<double, std::milli>(t2 - t1).count();
        t_enc  += duration<double, std::milli>(t3 - t2).count();

        if (i % 30 == 0)
            std::cout << "Frame " << i
                      << " grab:"   << duration<double, std::milli>(t1-t0).count()
                      << "ms debayer:" << duration<double, std::milli>(t2-t1).count()
                      << "ms enc:"     << duration<double, std::milli>(t3-t2).count() << "ms\n";
    }

    xiStopAcquisition(cam);
    xiCloseDevice(cam);
    fflush(ffmpeg);
    pclose(ffmpeg);

    double n = num_frames;
    double avg_g = t_grab/n, avg_p=t_proc/n, avg_e=t_enc/n;
    double tot = avg_g+avg_p+avg_e;
    std::cout << "\nAverages: grab " << avg_g << "ms, debayer " << avg_p
              << "ms, encode " << avg_e << "ms, total " << tot
              << "ms → " << 1000.0/tot << " FPS\n";
}

