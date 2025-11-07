// g++ xi_grab_half_preserve_bayer.cpp -o xi_grab_half_preserve_bayer     -I/opt/XIMEA/include -lm3api     $(pkg-config --cflags --libs libavcodec libavformat libavutil libswscale)     -pthread
// OK SO WITH DEBAYER HALF COLOR YOU GET A GOOD IMAGE AFTER RESET OF CAMERA
// SETTINGS IF YOU DO DEBAYER HALF COLOR WITH GBRG
// AND THEN PASS THAT TO FFMPEG TO BECOME BGR24
// SO ACTUALLY CAMERA IS GIVING GBRG, CODE MAKES BGR24...
// NOTE IMAGE LOOKS GREEN STILL NEEDS A WHITE BALANCE OF R=1.28, G=1, B=1.43 or more R.
// ffmpeg -f rawvideo -s 1024x352 -pix_fmt bayer_gbrg8 -i input.avi ...

/*
    ffcmd = "ffmpeg -i \"%s\" -f image2pipe -pix_fmt gray -vcodec rawvideo - | ffmpeg -loglevel quiet -r %d -f rawvideo -s 2048x700 -pix_fmt bayer_gbrg8 -i pipe:0 -y -pix_fmt yuv420p -vf colorchannelmixer=1.05:0:0:0:0:1.0:0:0:0:0:2.0:0 -crf 16 \"%s\"" % (vidfile,pfps,os.path.join(os.path.split(vidfile)[0],targetfn))
    
ffmpeg -i xi_stream_halfpreserve_raw8.avi_losslessmovie_raw_codec.avi -f image2pipe -pix_fmt gray -vcodec rawvideo - | ffmpeg -f rawvideo -s 1024x352 -pix_fmt bayer_gbrg8 -i pipe:0 -y -pix_fmt yuv420p -vf colorchannelmixer=1.05:0:0:0:0:1.0:0:0:0:0:2.0:0 -crf 16 testout.mp4

ffmpeg -i xi_stream_halfpreserve_raw8.avi_losslessmovie_raw_codec.avi -f image2pipe -pix_fmt gray -vcodec rawvideo - | ffmpeg -f rawvideo -s 1024x352 -pix_fmt bayer_gbrg8 -i pipe:0 -y -pix_fmt yuv420p -crf 16 testout.mp4

*/
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

// pattern: GBRG, BGGR, RGGB, GRBG
enum class BayerPattern { GBRG, BGGR, RGGB, GRBG };

// Take RAW8 Bayer input (stride bytes per row), downsample by 2x keeping Bayer mosaic.
// Outputs RAW8 half-res with the SAME Bayer pattern.
void bayer_half_preserve_cfa(const uint8_t* in, int in_w, int in_h, int stride,
                             uint8_t* out, int& out_w, int& out_h,
                             BayerPattern pattern)
{
    // Even dimensions
    int iw = in_w & ~1;
    int ih = in_h & ~1;
    int ow = iw / 2;
    int oh = ih / 2;

    auto idx = [&](int y, int x){ return y * stride + x; };
    auto oidx = [&](int y, int x){ return y * ow + x; }; // RAW8 contiguous, no padding in OUT

    // For each 2x2 block in input, produce ONE pixel in output that preserves CFA
    for (int y = 0; y < ih; y += 2) {
        uint8_t* orow = out + (y/2)*ow;
        for (int x = 0; x < iw; x += 2) {
            // 2x2 block
            uint8_t tl = in[idx(y+0, x+0)];
            uint8_t tr = in[idx(y+0, x+1)];
            uint8_t bl = in[idx(y+1, x+0)];
            uint8_t br = in[idx(y+1, x+1)];

            uint8_t G, R, B;

            // Map by pattern at top-left (x,y)
            switch (pattern) {
                case BayerPattern::GBRG: // row0:G B / row1:R G
                    G = uint8_t((int(tl) + int(br)) >> 1);
                    R = bl;
                    B = tr;
                    break;
                case BayerPattern::BGGR: // row0:B G / row1:G R
                    G = uint8_t((int(tr) + int(bl)) >> 1);
                    R = br;
                    B = tl;
                    break;
                case BayerPattern::RGGB: // row0:R G / row1:G B
                    G = uint8_t((int(tr) + int(bl)) >> 1);
                    R = tl;
                    B = br;
                    break;
                case BayerPattern::GRBG: // row0:G R / row1:B G
                    G = uint8_t((int(tl) + int(br)) >> 1);
                    R = tr;
                    B = bl;
                    break;
            }

            // We must output ONE Bayer sample at (y/2,x/2) consistent with the pattern.
            // Choose to place the **top-left** site of the pattern in the output:
            // For patterns where top-left is G (GBRG/GRBG), write G.
            // For RGGB -> write R; for BGGR -> write B.
            uint8_t v;
            switch (pattern) {
                case BayerPattern::GBRG: v = G; break; // TL is G
                case BayerPattern::GRBG: v = G; break; // TL is G
                case BayerPattern::RGGB: v = R; break; // TL is R
                case BayerPattern::BGGR: v = B; break; // TL is B
            }

            orow[x/2] = v;
        }
    }

    out_w = ow;
    out_h = oh;
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
    BayerPattern pattern = BayerPattern::GBRG;  // your camera
    const char* outfile = "xi_stream_halfpreserve_raw8.avi";

    if (argc > 1) num_frames = std::atoi(argv[1]);
    std::cout << "Capturing " << num_frames << " frames using pattern GBRG\n";

    std::signal(SIGPIPE, SIG_IGN);

    HANDLE cam = nullptr;
    XI_RETURN stat = xiOpenDevice(0, &cam);
    if (stat != XI_OK) {
        std::cerr << "xiOpenDevice failed\n";
        return 1;
    }

    int width = 2048, height = 704;
    xiSetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(cam, XI_PRM_WIDTH, width);
    xiSetParamInt(cam, XI_PRM_HEIGHT, height);
    xiSetParamInt(cam, XI_PRM_EXPOSURE, 2000);

    print_cam_cfg(cam);

    XI_IMG img = {};
    img.size = sizeof(XI_IMG);

    int actual_w = 0, actual_h = 0;
    xiGetParamInt(cam, XI_PRM_WIDTH, &actual_w);
    xiGetParamInt(cam, XI_PRM_HEIGHT, &actual_h);
    actual_w &= ~1;
    actual_h &= ~1;
    int out_w = actual_w / 2;
    int out_h = actual_h / 2;

    size_t out_bytes = static_cast<size_t>(out_w) * out_h;  // RAW8 (1 byte/pixel)

    std::cout << "==== Camera ROI and Half-Preserve Config ====\n";
    std::cout << "Sensor ROI: " << actual_w << " x " << actual_h << "\n";
    std::cout << "Half-preserve Output: " << out_w << " x " << out_h
              << " (" << out_bytes << " bytes per frame)\n";
    std::cout << "================================================\n";

    std::vector<uint8_t> out_buf(out_bytes);

    // --- Uncompressed RAW8 AVI output ---
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/ffmpeg -f rawvideo -pix_fmt gray -s %dx%d "
        "-i pipe:0 -y -threads 0 -c:v rawvideo "
        "-pix_fmt gray -framerate 100 %s",
        out_w, out_h, outfile);

    FILE* ffmpeg = popen(cmd, "w");
    if (!ffmpeg) {
        perror("popen");
        xiCloseDevice(cam);
        return 1;
    }

    xiStartAcquisition(cam);

    int padx = 0;
    if (xiGetParamInt(cam, XI_PRM_PADDING_X, &padx) != XI_OK)
        padx = 0;
    const int stride = width + padx;

    double t_grab = 0, t_proc = 0, t_enc = 0;

    for (int i = 0; i < num_frames; ++i) {
        auto t0 = high_resolution_clock::now();

        stat = xiGetImage(cam, 100, &img);
        if (stat != XI_OK || !img.bp) {
            std::cerr << "xiGetImage failed\n";
            break;
        }
        auto t1 = high_resolution_clock::now();

        // --- Decimate while preserving Bayer CFA pattern ---
        bayer_half_preserve_cfa(static_cast<uint8_t*>(img.bp),
                                img.width, img.height, stride,
                                out_buf.data(),
                                out_w, out_h,
                                pattern);

        auto t2 = high_resolution_clock::now();

        size_t written = fwrite(out_buf.data(), 1, out_bytes, ffmpeg);
        if (written != out_bytes) {
            std::cerr << "pipe write failed\n";
            break;
        }

        auto t3 = high_resolution_clock::now();

        // --- accumulate timings ---
        t_grab += duration<double, std::milli>(t1 - t0).count();
        t_proc += duration<double, std::milli>(t2 - t1).count();
        t_enc  += duration<double, std::milli>(t3 - t2).count();

        if (i % 30 == 0)
            std::cout << "Frame " << i
                      << " grab:"   << duration<double, std::milli>(t1 - t0).count()
                      << "ms half-preserve:" << duration<double, std::milli>(t2 - t1).count()
                      << "ms write:" << duration<double, std::milli>(t3 - t2).count()
                      << "ms\n";
    }


    xiStopAcquisition(cam);
    xiCloseDevice(cam);
    fflush(ffmpeg);
    pclose(ffmpeg);

    double n = num_frames;
    double avg_g = t_grab/n, avg_p = t_proc/n, avg_e = t_enc/n;
    double tot = avg_g + avg_p + avg_e;
    std::cout << "\nAverages: grab " << avg_g << "ms, process " << avg_p
              << "ms, write " << avg_e << "ms, total " << tot
              << "ms → " << 1000.0/tot << " FPS\n";
}


