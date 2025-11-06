#include <iostream>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <csignal>
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

    int width = 2048, height = 700;
    xiSetParamInt(cam, XI_PRM_WIDTH, width);
    xiSetParamInt(cam, XI_PRM_HEIGHT, height);
    xiSetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(cam, XI_PRM_EXPOSURE, 2000);
    xiSetParamInt(cam, XI_PRM_BUFFERS_QUEUE_SIZE, 2);

    print_cam_cfg(cam);

    XI_IMG img = {};
    img.size = sizeof(XI_IMG);

    int actual_w=0, actual_h=0;
    xiGetParamInt(cam, XI_PRM_WIDTH, &actual_w);
    xiGetParamInt(cam, XI_PRM_HEIGHT, &actual_h);
    actual_w &= ~1; actual_h &= ~1;
    int out_w = actual_w / 2, out_h = actual_h / 2;
    size_t rgb_bytes = static_cast<size_t>(out_w) * out_h * 3;

    std::vector<uint8_t> debayer_buf(rgb_bytes);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -f rawvideo -pix_fmt bgr24 -s %dx%d -r 100 "
             "-i pipe:0 -y -threads 0 -preset ultrafast "
             "-c:v libx264 -crf 18 -pix_fmt yuv420p %s",
             out_w, out_h, outfile);
    FILE* ffmpeg = popen(cmd, "w");
    if (!ffmpeg) { perror("popen"); xiCloseDevice(cam); return 1; }

    xiStartAcquisition(cam);

    double t_grab=0, t_proc=0, t_enc=0;
    for (int i=0; i<num_frames; ++i) {
        auto t0 = high_resolution_clock::now();
        stat = xiGetImage(cam, 100, &img);
        if (stat != XI_OK || !img.bp) { std::cerr << "xiGetImage failed\n"; break; }
        auto t1 = high_resolution_clock::now();

        int padx=0;
        xiGetParamInt(cam, XI_PRM_PADDING_X, &padx);
        int stride = img.width + padx;

        debayer_half_color(static_cast<uint8_t*>(img.bp),
                           img.width, img.height, stride,
                           debayer_buf.data(), pattern);
        auto t2 = high_resolution_clock::now();

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

