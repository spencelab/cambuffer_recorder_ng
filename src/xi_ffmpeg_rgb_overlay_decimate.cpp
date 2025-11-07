/*
Build:
    g++ xi_ffmpeg_rgb_overlay_decimate.cpp -O2 -o xi_ffmpeg_rgb_overlay_decimate \
        -I/opt/XIMEA/include -lm3api `pkg-config --cflags --libs opencv4` -pthread

Run examples:
    ./xi_ffmpeg_rgb_overlay_decimate 2048 700 2000 out_full.mp4
    ./xi_ffmpeg_rgb_overlay_decimate 2048 700 2000 out_half.mp4 half
*/

#include <m3api/xiApi.h>
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <chrono>
#include <iostream>

using namespace std;
using namespace std::chrono;

static volatile bool stop_flag = false;
void signal_handler(int sig) {
    stop_flag = true;
    fprintf(stderr, "Exiting normally, received signal %d.\n", sig);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        cerr << "usage: " << argv[0] << " width height exposure output.mp4 [half]\n";
        return 1;
    }

    int W = atoi(argv[1]);
    int H = atoi(argv[2]);
    int EXP = atoi(argv[3]);
    string outfile = argv[4];
    bool halfres = (argc > 5 && string(argv[5]) == "half");

    signal(SIGINT, signal_handler);
    signal(SIGPIPE, signal_handler);

    HANDLE cam = nullptr;
    XI_RETURN stat = xiOpenDevice(0, &cam);
    if (stat != XI_OK) { cerr << "xiOpenDevice failed\n"; return 1; }

    xiSetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, XI_RGB24);
    xiSetParamInt(cam, XI_PRM_WIDTH, W);
    xiSetParamInt(cam, XI_PRM_HEIGHT, H);
    xiSetParamInt(cam, XI_PRM_EXPOSURE, EXP);

    cout << "Opened camera, " << (halfres ? "HALF-res decimation" : "FULL-res mode") << endl;

    XI_IMG img{};
    img.size = sizeof(XI_IMG);
    xiStartAcquisition(cam);

    int outW = halfres ? W / 2 : W;
    int outH = halfres ? H / 2 : H;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "/usr/bin/ffmpeg -f rawvideo -pix_fmt bgr24 -s %dx%d -r 120 "
             "-i pipe:0 -y -threads 0 -preset ultrafast "
             "-c:v libx264 -crf 16 -pix_fmt yuv420p %s",
             outW, outH, outfile.c_str());
    FILE* ffmpeg = popen(cmd, "w");
    if (!ffmpeg) { perror("popen"); xiCloseDevice(cam); return 1; }

    vector<uint8_t> framebuf(W * H * 3);
    cv::Mat frame(H, W, CV_8UC3, framebuf.data());
    cv::Mat outframe(outH, outW, CV_8UC3);

    uint64_t fcount = 0;
    double grab_ms_sum = 0, overlay_ms_sum = 0, encode_ms_sum = 0;

    auto t_start = high_resolution_clock::now();

    while (!stop_flag) {
        auto t0 = high_resolution_clock::now();
        stat = xiGetImage(cam, 100, &img);
        if (stat != XI_OK || !img.bp) break;
        memcpy(framebuf.data(), img.bp, W * H * 3);
        auto t1 = high_resolution_clock::now();

        // --- optional additive decimate ---
        if (halfres) {
            cv::resize(frame, outframe, outframe.size(), 0, 0, cv::INTER_AREA);
            // brighten slightly since averaging darkens
            outframe.convertTo(outframe, -1, 1.7, 0); // scale pixel values ×1.7
        } else {
            outframe = frame;
        }

        // --- overlay frame number and timestamp ---
        auto now = system_clock::now();
        auto ts_us = duration_cast<microseconds>(now.time_since_epoch()).count();
        char textL[64], textR[64];
        snprintf(textL, sizeof(textL), "F%06llu", (unsigned long long)fcount);
        snprintf(textR, sizeof(textR), "%.6f", ts_us / 1e6);
        int baseline = 0;
        cv::Size sz = cv::getTextSize(textR, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        cv::putText(outframe, textL, cv::Point(5, 20),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0,255,0), 1, cv::LINE_AA);
        cv::putText(outframe, textR, cv::Point(outframe.cols - sz.width - 32, 20),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0,0,255), 1, cv::LINE_AA);
        auto t2 = high_resolution_clock::now();

        // --- write to ffmpeg pipe ---
        size_t written = fwrite(outframe.data, 1, outW * outH * 3, ffmpeg);
        if (written != (size_t)(outW * outH * 3)) {
            cerr << "pipe write failed (ffmpeg closed?)\n";
            stop_flag = true;
            break;
        }
        auto t3 = high_resolution_clock::now();

        grab_ms_sum += duration<double, milli>(t1 - t0).count();
        overlay_ms_sum += duration<double, milli>(t2 - t1).count();
        encode_ms_sum += duration<double, milli>(t3 - t2).count();

        if (fcount % 50 == 0 && fcount > 0) {
            double avg_g = grab_ms_sum / fcount;
            double avg_o = overlay_ms_sum / fcount;
            double avg_e = encode_ms_sum / fcount;
            double total = avg_g + avg_o + avg_e;
            cout << "F" << fcount
                 << " grab=" << avg_g << "ms overlay=" << avg_o
                 << "ms encode=" << avg_e << "ms → " << 1000.0/total << " FPS\n";
        }
        fcount++;
    }

    // cleanup
    xiStopAcquisition(cam);
    xiCloseDevice(cam);
    fflush(ffmpeg);
    pclose(ffmpeg);

    auto t_end = high_resolution_clock::now();
    double elapsed_s = duration<double>(t_end - t_start).count();
    double avg_g = grab_ms_sum / fcount;
    double avg_o = overlay_ms_sum / fcount;
    double avg_e = encode_ms_sum / fcount;
    double total = avg_g + avg_o + avg_e;

    cout << "\nAverage timings (" << fcount << " frames):\n"
         << "grab=" << avg_g << "ms overlay=" << avg_o
         << "ms encode=" << avg_e << "ms total=" << total
         << "ms → " << 1000.0/total << " FPS\n"
         << "Elapsed real time: " << elapsed_s << " s\n";
}

