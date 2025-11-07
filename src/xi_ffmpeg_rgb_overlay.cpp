/*
sudo apt install libopencv-dev

g++ xi_ffmpeg_rgb_overlay.cpp -O2 -o xi_ffmpeg_rgb_overlay \
    -I/opt/XIMEA/include -lm3api \
    $(pkg-config --cflags --libs opencv4) \
    -pthread

./xi_ffmpeg_rgb_overlay 2048 1088 1000 out_rgb_overlay.mp4


*/
#include <m3api/xiApi.h>
#include <opencv2/opencv.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

using namespace std;
using namespace std::chrono;

static bool stop_flag = false;
void signal_handler(int) { stop_flag = true; }

// Utility: formatted timestamp string
static string timestamp_str() {
    auto now = system_clock::now();
    auto sec = time_point_cast<seconds>(now);
    auto nsec = duration_cast<nanoseconds>(now - sec);
    time_t t = system_clock::to_time_t(now);
    char buf[64];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
    char out[80];
    snprintf(out, sizeof(out), "%s.%03lld", buf, (long long)(nsec.count() / 1e6));
    return out;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        cerr << "Usage: " << argv[0]
             << " width height exposure outfile.mp4\n";
        return 1;
    }
    int W = atoi(argv[1]);
    int H = atoi(argv[2]);
    int EXP = atoi(argv[3]);
    string outfile = argv[4];

    signal(SIGINT, signal_handler);
    signal(SIGPIPE, signal_handler);
    
    // --- Open camera ---
    HANDLE cam = nullptr;
    if (xiOpenDevice(0, &cam) != XI_OK) {
        cerr << "Failed to open XIMEA camera.\n";
        return 1;
    }

    xiSetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, XI_RGB24);
    xiSetParamInt(cam, XI_PRM_WIDTH, W);
    xiSetParamInt(cam, XI_PRM_HEIGHT, H);
    xiSetParamInt(cam, XI_PRM_EXPOSURE, EXP);
    xiSetParamInt(cam, XI_PRM_BUFFERS_QUEUE_SIZE, 4);
    xiStartAcquisition(cam);

    cout << "Streaming " << W << "x" << H << " RGB24 frames...\n";

    // --- Prepare ffmpeg pipe ---
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "/usr/bin/ffmpeg -f rawvideo -pix_fmt bgr24 -s %dx%d -r 120 "
             "-i pipe:0 -y -threads 0 -preset ultrafast "
             "-c:v libx264 -crf 18 -pix_fmt yuv420p %s",
             W, H, outfile.c_str());

    FILE *ffmpeg = popen(cmd, "w");
    if (!ffmpeg) {
        perror("popen");
        xiCloseDevice(cam);
        return 1;
    }

    XI_IMG img{};
    img.size = sizeof(XI_IMG);
    uint64_t frame_idx = 0;

    double total_grab = 0, total_ovr = 0, total_enc = 0;
    int count = 0;

    while (!stop_flag) {
        auto t0 = high_resolution_clock::now();
        if (xiGetImage(cam, 100, &img) != XI_OK || !img.bp) {
            cerr << "xiGetImage failed or timed out.\n";
            break;
        }
        auto t1 = high_resolution_clock::now();

        cv::Mat frame(H, W, CV_8UC3, img.bp);
        auto t2 = high_resolution_clock::now();

        // Overlay frame index and timestamp
        string left_text = "F" + to_string(frame_idx);
        string right_text = timestamp_str();

        int baseline = 0;
        cv::Size left_sz = cv::getTextSize(left_text, cv::FONT_HERSHEY_SIMPLEX, 1.0, 2, &baseline);
        cv::Size right_sz = cv::getTextSize(right_text, cv::FONT_HERSHEY_SIMPLEX, 1.0, 2, &baseline);

        cv::putText(frame, left_text, cv::Point(10, 40),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(255, 255, 255), 2);
        cv::putText(frame, right_text,
                    cv::Point(W - right_sz.width - 10, 40),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(255, 255, 255), 2);

        auto t3 = high_resolution_clock::now();

        // Write to ffmpeg pipe
        size_t written = fwrite(frame.data, 1, W * H * 3, ffmpeg);
        if (written != (size_t)W * H * 3) {
            cerr << "pipe write failed\n";
            break;
        }
        auto t4 = high_resolution_clock::now();

        total_grab += duration<double, milli>(t1 - t0).count();
        total_ovr += duration<double, milli>(t3 - t2).count();
        total_enc += duration<double, milli>(t4 - t3).count();
        frame_idx++;
        count++;

        if (count % 50 == 0) {
            cout << "F" << frame_idx
                 << " grab:" << duration<double, milli>(t1 - t0).count()
                 << "ms overlay:" << duration<double, milli>(t3 - t2).count()
                 << "ms encode:" << duration<double, milli>(t4 - t3).count()
                 << "ms\n";
        }
    }

    xiStopAcquisition(cam);
    xiCloseDevice(cam);
    fflush(ffmpeg);
    pclose(ffmpeg);

    double n = count;
    cout << fixed << setprecision(3);
    cout << "\nAverage timings (" << n << " frames):\n"
         << "grab=" << total_grab / n << "ms overlay=" << total_ovr / n
         << "ms encode=" << total_enc / n
         << "ms total=" << (total_grab + total_ovr + total_enc) / n
         << "ms → " << 1000.0 / ((total_grab + total_ovr + total_enc) / n)
         << " FPS\n";
}

