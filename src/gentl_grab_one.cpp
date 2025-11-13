/*
g++ gentl_grab_one.cpp -I/opt/XIMEA/samples/_libs/GenTL -ldl `pkg-config --cflags --libs opencv4` -o gentl_grab_one

if opencv were in conda:
g++ gentl_grab_one.cpp -ldl \
    -I$CONDA_PREFIX/include \
    -L$CONDA_PREFIX/lib -lopencv_core -lopencv_imgcodecs -lopencv_imgproc \
    -o gentl_grab_one


./gentl_grab_one


*/
#include <iostream>
#include <dlfcn.h>
#include <cstdlib>
#include <string>
#include <vector>

#include <GenTL_v1_5.h>
#include <opencv2/opencv.hpp>

using namespace std;

static void check(GenTL::GC_ERROR err, const char* msg)
{
    if (err != GenTL::GC_ERR_SUCCESS) {
        cerr << "ERROR: " << msg << " (code=" << err << ")" << endl;
        exit(1);
    }
}

template<typename T>
static void load_or_die(T fn, const char* name)
{
    if (!fn) {
        std::cerr << "Failed to load symbol: " << name << std::endl;
        exit(1);
    }
}


int main()
{
    const char* base = getenv("GENICAM_GENTL64_PATH");
    if (!base) {
        cerr << "GENICAM_GENTL64_PATH not set" << endl;
        return 1;
    }

    string cti_path = string(base) + "/ximea.gentl.cti";
    cout << "Loading CTI: " << cti_path << endl;

    void* lib = dlopen(cti_path.c_str(), RTLD_NOW);
    if (!lib) {
        cerr << "dlopen failed: " << dlerror() << endl;
        return 1;
    }

    // Load required GenTL symbols
    auto pGCInitLib   = (GenTL::PGCInitLib)   dlsym(lib, "GCInitLib");
    auto pGCCloseLib  = (GenTL::PGCCloseLib)  dlsym(lib, "GCCloseLib");
    auto pTLOpen      = (GenTL::PTLOpen)      dlsym(lib, "TLOpen");
    auto pTLClose     = (GenTL::PTLClose)     dlsym(lib, "TLClose");
    auto pTLGetNumInterfaces = (GenTL::PTLGetNumInterfaces) dlsym(lib, "TLGetNumInterfaces");
    auto pTLGetInterfaceID   = (GenTL::PTLGetInterfaceID)   dlsym(lib, "TLGetInterfaceID");
    auto pTLOpenInterface    = (GenTL::PTLOpenInterface)    dlsym(lib, "TLOpenInterface");
    auto pIFGetNumDevices    = (GenTL::PIFGetNumDevices)    dlsym(lib, "IFGetNumDevices");
    auto pIFGetDeviceID      = (GenTL::PIFGetDeviceID)      dlsym(lib, "IFGetDeviceID");
    auto pIFOpenDevice       = (GenTL::PIFOpenDevice)       dlsym(lib, "IFOpenDevice");

    auto pDevGetNumDataStreams = (GenTL::PDevGetNumDataStreams) dlsym(lib, "DevGetNumDataStreams");
    auto pDevGetDataStreamID   = (GenTL::PDevGetDataStreamID)   dlsym(lib, "DevGetDataStreamID");
    auto pDevOpenDataStream    = (GenTL::PDevOpenDataStream)    dlsym(lib, "DevOpenDataStream");
    auto pDevClose             = (GenTL::PDevClose)             dlsym(lib, "DevClose");

    auto pDSAnnounceBuffer     = (GenTL::PDSAnnounceBuffer)     dlsym(lib, "DSAnnounceBuffer");
    auto pDSQueueBuffer        = (GenTL::PDSQueueBuffer)        dlsym(lib, "DSQueueBuffer");
    auto pDSStartAcquisition   = (GenTL::PDSStartAcquisition)   dlsym(lib, "DSStartAcquisition");
    auto pDSStopAcquisition    = (GenTL::PDSStopAcquisition)    dlsym(lib, "DSStopAcquisition");
    auto pDSGetBufferInfo      = (GenTL::PDSGetBufferInfo)      dlsym(lib, "DSGetBufferInfo");
    auto pDSGetBufferID        = (GenTL::PDSGetBufferID)        dlsym(lib, "DSGetBufferID");
    auto pDSRevokeBuffer       = (GenTL::PDSRevokeBuffer)       dlsym(lib, "DSRevokeBuffer");
    auto pDSClose              = (GenTL::PDSClose)              dlsym(lib, "DSClose");

    load_or_die(pGCInitLib, "GCInitLib");
    check(pGCInitLib(), "GCInitLib");

    GenTL::TL_HANDLE hTL = nullptr;
    check(pTLOpen(&hTL), "TLOpen");

    uint32_t num_ifaces = 0;
    check(pTLGetNumInterfaces(hTL, &num_ifaces), "TLGetNumInterfaces");

    cout << "Interfaces: " << num_ifaces << endl;

    GenTL::DEV_HANDLE hDev = nullptr;
    GenTL::DS_HANDLE  hDS  = nullptr;

    // ---- Find first device on any interface ----
    for (uint32_t i = 0; i < num_ifaces; i++) {
        char iface_id[512] = {0};
        size_t iface_len = sizeof(iface_id);

        check(pTLGetInterfaceID(hTL, i, iface_id, &iface_len), "TLGetInterfaceID");

        GenTL::IF_HANDLE hIF = nullptr;
        check(pTLOpenInterface(hTL, iface_id, &hIF), "TLOpenInterface");

        uint32_t num_devs = 0;
        check(pIFGetNumDevices(hIF, &num_devs), "IFGetNumDevices");

        if (num_devs > 0) {
            // Open first device
            char dev_id[512] = {0};
            size_t dev_len = sizeof(dev_id);
            check(pIFGetDeviceID(hIF, 0, dev_id, &dev_len), "IFGetDeviceID");

            cout << "Using Device: " << dev_id << endl;

            check(pIFOpenDevice(hIF, dev_id, GenTL::DEVICE_ACCESS_EXCLUSIVE, &hDev), "IFOpenDevice");

            uint32_t num_ds = 0;
            check(pDevGetNumDataStreams(hDev, &num_ds), "DevGetNumDataStreams");

            if (num_ds == 0) {
                cerr << "Device has no data streams." << endl;
                return 1;
            }

            char ds_id[128] = {0};
            size_t ds_len = sizeof(ds_id);
            check(pDevGetDataStreamID(hDev, 0, ds_id, &ds_len), "DevGetDataStreamID");

            check(pDevOpenDataStream(hDev, ds_id, &hDS), "DevOpenDataStream");

            break;
        }
    }

    if (!hDev || !hDS) {
        cerr << "No device found!" << endl;
        return 1;
    }

    // ---- Acquire one buffer ----
    const size_t BUF_SIZE = 2048 * 1536; // enough for one RAW8 frame
    vector<uint8_t> buffer(BUF_SIZE);
    GenTL::BUFFER_HANDLE hBuf;

    check(pDSAnnounceBuffer(hDS, buffer.data(), BUF_SIZE, nullptr, &hBuf), "DSAnnounceBuffer");
    check(pDSQueueBuffer(hDS, hBuf), "DSQueueBuffer");

    check(pDSStartAcquisition(hDS, GenTL::ACQ_START_FLAGS_DEFAULT, 1), "DSStartAcquisition");

    // Wait for buffer to fill
    GenTL::INFO_DATATYPE type;
    size_t filled_size = sizeof(size_t);
    size_t bytes_filled = 0;

    do {
        check(pDSGetBufferInfo(hDS, hBuf, GenTL::BUFFER_INFO_SIZE_FILLED,
                               &type, &bytes_filled, &filled_size),
              "DSGetBufferInfo");
    } while (bytes_filled == 0);

    cout << "Acquired " << bytes_filled << " bytes" << endl;

    // Stop acquisition
    check(pDSStopAcquisition(hDS, GenTL::ACQ_STOP_FLAGS_DEFAULT), "DSStopAcquisition");

    // ---- Convert to OpenCV Mat ----
    // Adjust width/height manually for your camera settings
    const int WIDTH = 2048;
    const int HEIGHT = bytes_filled / WIDTH;

    cv::Mat img(HEIGHT, WIDTH, CV_8UC1, buffer.data());
    cv::imwrite("gentl_frame.png", img);

    cout << "Saved gentl_frame.png" << endl;

    // Cleanup
    void* p = nullptr;
    check(pDSRevokeBuffer(hDS, hBuf, &p, nullptr), "DSRevokeBuffer");
    check(pDSClose(hDS), "DSClose");

    check(pDevClose(hDev), "DevClose");
    check(pTLClose(hTL), "TLClose");

    pGCCloseLib();
    dlclose(lib);

    return 0;
}



