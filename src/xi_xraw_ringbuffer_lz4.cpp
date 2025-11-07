/*
build:

g++ xi_xraw_ringbuffer_lz4.cpp -O2 -o xi_xraw_ringbuffer_lz4 \
    -I/opt/XIMEA/include -lm3api -llz4 -pthread

run:

./xi_xraw_ringbuffer_lz4 2048 700 2000 10000 100 ringtest_lz4

*/

#include <m3api/xiApi.h>
#include <lz4frame.h>

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>

using namespace std;
using namespace std::chrono;

struct FrameSlot {
    std::vector<uint8_t> data;  // compressed data
    uint64_t ts_ns;
    uint64_t frame_index;
};

struct FileHeader {
    uint32_t magic = 0x58524157;  // 'XRAW'
    uint16_t ver = 2;             // version 2 for LZ4
    uint16_t hdr_sz = sizeof(FileHeader);
    uint64_t start_ns;
    uint32_t w, h, stride;
};
struct FrameHeader {
    uint32_t magic = 0x5842494E;  // 'XBIN'
    uint16_t ver = 2;
    uint16_t hdr_sz = sizeof(FrameHeader);
    uint64_t frame_index, ts_ns;
    uint32_t bytes;  // compressed byte count
};

static atomic<bool> stop_flag = false;
static atomic<size_t> dropped_frames = 0;

int main(int argc, char **argv) {
    if (argc < 7) {
        cerr << "usage: " << argv[0] << " width height exposure nframes bufsize prefix\n";
        return 1;
    }
    int W = atoi(argv[1]), H = atoi(argv[2]), EXP = atoi(argv[3]), N = atoi(argv[4]), BUF = atoi(argv[5]);
    string prefix = argv[6];

    HANDLE cam = nullptr;
    if (xiOpenDevice(0, &cam) != XI_OK) {
        cerr << "xiOpenDevice fail\n";
        return 1;
    }
    xiSetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(cam, XI_PRM_WIDTH, W);
    xiSetParamInt(cam, XI_PRM_HEIGHT, H);
    xiSetParamInt(cam, XI_PRM_EXPOSURE, EXP);
    {
        printf("Turning hardware trigger on.\n");
        xiSetParamInt(cam, XI_PRM_TRG_SOURCE, XI_TRG_EDGE_RISING);
        xiSetParamInt(cam, XI_PRM_TRG_SELECTOR, XI_TRG_SEL_FRAME_START);
        xiSetParamInt(cam, XI_PRM_GPI_SELECTOR, 1);
        xiSetParamInt(cam, XI_PRM_GPI_MODE, XI_GPI_TRIGGER);
    }
    xiStartAcquisition(cam);

    deque<FrameSlot> q;
    mutex mtx;
    condition_variable cv;
    FILE *fp = fopen((prefix + ".xraw").c_str(), "wb");
    FileHeader fh;
    fh.start_ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    fh.w = W;
    fh.h = H;
    fh.stride = W;
    fwrite(&fh, 1, sizeof(fh), fp);

    atomic<uint64_t> next_index = 0;

    // --- writer thread ---
    thread writer([&] {
        while (!stop_flag || !q.empty()) {
            FrameSlot slot;
            {
                unique_lock<mutex> lk(mtx);
                cv.wait(lk, [&] { return !q.empty() || stop_flag; });
                if (q.empty())
                    continue;
                slot = std::move(q.front());
                q.pop_front();
            }
            FrameHeader h{};
            h.frame_index = slot.frame_index;
            h.ts_ns = slot.ts_ns;
            h.bytes = slot.data.size();
            fwrite(&h, 1, sizeof(h), fp);
            fwrite(slot.data.data(), 1, slot.data.size(), fp);
            fflush(fp);
        }
    });

    XI_IMG img{};
    img.size = sizeof(XI_IMG);

    // --- grab loop ---
    for (int i = 0; i < N && !stop_flag; i++) {
        auto t0 = high_resolution_clock::now();
        if (xiGetImage(cam, 100, &img) != XI_OK)
            break;
        auto t1 = high_resolution_clock::now();

        uint8_t *src = static_cast<uint8_t *>(img.bp);
        size_t raw_size = (size_t)W * H;

        // LZ4 compression
        std::vector<uint8_t> compressed(LZ4F_compressFrameBound(raw_size, nullptr));
        size_t comp_size = LZ4F_compressFrame(compressed.data(), compressed.size(), src, raw_size, nullptr);
        if (LZ4F_isError(comp_size)) {
            cerr << "LZ4 compression error: " << LZ4F_getErrorName(comp_size) << endl;
            continue;
        }
        compressed.resize(comp_size);

        FrameSlot slot;
        slot.ts_ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        slot.frame_index = next_index++;
        slot.data = std::move(compressed);

        auto t2 = high_resolution_clock::now();

        {
            unique_lock<mutex> lk(mtx);
            if (q.size() < BUF) {
                q.push_back(std::move(slot));
                cv.notify_one();
            } else {
                dropped_frames++;
            }
        }

        auto grab_ms = duration<double, milli>(t1 - t0).count();
        auto comp_ms = duration<double, milli>(t2 - t1).count();
        if (i % 100 == 0) {
            size_t qdepth;
            { lock_guard<mutex> lk(mtx); qdepth = q.size(); }
            double ratio = (double)raw_size / comp_size;
            cout << "F" << i << " grab:" << grab_ms << "ms compress:" << comp_ms
                 << "ms qdepth=" << qdepth << " ratio=" << fixed << setprecision(2) << ratio << "x\n";
        }
    }

    stop_flag = true;
    cv.notify_all();
    writer.join();

    xiStopAcquisition(cam);
    xiCloseDevice(cam);
    fclose(fp);
    cout << "Dropped frames: " << dropped_frames.load() << "\n";
}

