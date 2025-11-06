#pragma once
#include "cambuffer_recorder_ng/ICamera.hpp"
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <atomic> 

namespace cambuffer_recorder_ng {

/**
 * Vendor-neutral GenTL camera consumer.
 * Supports both:
 *  - Full GC* hierarchy (System → Interface → Device → DataStream)
 *  - TL-only hierarchy (TLOpen → Interface → Device → DataStream), used by XIMEA
 */
class GenTLCamera : public ICamera {
public:
    GenTLCamera() = default;
    ~GenTLCamera() { close(); }

    void open(int device_index = 0) override { open("", device_index); }  // <── add this
    void open(const std::string& cti_path, int device_index = 0);         // main entry point

    void close() override;
    void start() override;
    void stop() override;

    bool grab(uint8_t*& data, size_t& size, uint64_t& ts,
              int& width, int& height, int& stride, int timeout_ms = 100) override;


private:
    // dynamic library handle
    void* lib_ = nullptr;

    // opened handles
    void* hTL_ = nullptr;     // TL-level handle (XIMEA path)
    void* hSystem_ = nullptr; // GC path only
    void* hIF_ = nullptr;
    void* hDev_ = nullptr;
    void* hDS_ = nullptr;

    std::vector<uint8_t> buffer_;
    std::atomic<bool> running_{false};
    bool use_gc_path_ = false; // true if GC* symbols were found/used

    struct BufRec { void* hBuf = nullptr; void* pData = nullptr; size_t sz = 0; };
    std::vector<BufRec> bufs_;

    // ---- Function pointer typedefs ----
    // Library
    using F_GCInitLib = int(*)();
    using F_GCCloseLib = int(*)();
    F_GCInitLib  GCInitLib  = nullptr;
    F_GCCloseLib GCCloseLib = nullptr;

    // ----- GC-level (may be absent on XIMEA) -----
    using F_GCGetNumSystems = int(*)(uint32_t*);
    using F_GCGetSystemID   = int(*)(uint32_t, char*, size_t*);
    using F_GCOpenSystem    = int(*)(const char*, void**);
    using F_GCCloseSystem   = int(*)(void*);

    using F_SysGetNumInterfaces = int(*)(void*, uint32_t*);
    using F_SysGetInterfaceID   = int(*)(void*, uint32_t, char*, size_t*);
    using F_SysOpenInterface    = int(*)(void*, const char*, void**, void*);
    using F_SysCloseInterface   = int(*)(void*);

    F_GCGetNumSystems    GCGetNumSystems = nullptr;
    F_GCGetSystemID      GCGetSystemID   = nullptr;
    F_GCOpenSystem       GCOpenSystem    = nullptr;
    F_GCCloseSystem      GCCloseSystem   = nullptr;
    F_SysGetNumInterfaces SysGetNumInterfaces = nullptr;
    F_SysGetInterfaceID   SysGetInterfaceID   = nullptr;
    F_SysOpenInterface    SysOpenInterface    = nullptr;
    F_SysCloseInterface   SysCloseInterface   = nullptr;

    // ----- TL-level (XIMEA) -----
    using F_TLOpen             = int(*)(void**);
    using F_TLClose            = int(*)(void*);
    using F_TLGetNumInterfaces = int(*)(void*, uint32_t*);
    using F_TLGetInterfaceID   = int(*)(void*, uint32_t, char*, size_t*);
    F_TLOpen             TLOpen = nullptr;
    F_TLClose            TLClose = nullptr;
    F_TLGetNumInterfaces TLGetNumInterfaces = nullptr;
    F_TLGetInterfaceID   TLGetInterfaceID   = nullptr;

    // Interface / Device
    using F_IFGetNumDevices = int(*)(void*, uint32_t*);
    using F_IFGetDeviceID   = int(*)(void*, uint32_t, char*, size_t*);
    using F_IFOpenDevice    = int(*)(void*, const char*, void**, uint32_t);
    using F_IFClose         = int(*)(void*);
    F_IFGetNumDevices IFGetNumDevices = nullptr;
    F_IFGetDeviceID   IFGetDeviceID   = nullptr;
    F_IFOpenDevice    IFOpenDevice    = nullptr;
    F_IFClose         IFClose         = nullptr;

    // Device → DataStream
    using F_DevGetNumDataStreams = int(*)(void*, uint32_t*);
    using F_DevGetDataStreamID   = int(*)(void*, uint32_t, char*, size_t*);
    using F_DevOpenDataStream    = int(*)(void*, const char*, void**);
    using F_DevClose             = int(*)(void*);
    F_DevGetNumDataStreams DevGetNumDataStreams = nullptr;
    F_DevGetDataStreamID   DevGetDataStreamID   = nullptr;
    F_DevOpenDataStream    DevOpenDataStream    = nullptr;
    F_DevClose             DevClose             = nullptr;

    // DataStream
    using F_DSAnnounceBuffer   = int(*)(void*, void*, size_t, void**);
    using F_DSQueueBuffer      = int(*)(void*, void*);
    using F_DSStartAcquisition = int(*)(void*, uint64_t, uint32_t);
    using F_DSStopAcquisition  = int(*)(void*, uint32_t);
    using F_DSGetBuffer        = int(*)(void*, void**, uint32_t);
    using F_DSRevokeBuffer     = int(*)(void*, void*, void*, void*);
    using F_DSClose            = int(*)(void*);
    F_DSAnnounceBuffer   DSAnnounceBuffer = nullptr;
    F_DSQueueBuffer      DSQueueBuffer    = nullptr;
    F_DSStartAcquisition DSStartAcquisition = nullptr;
    F_DSStopAcquisition  DSStopAcquisition  = nullptr;
    F_DSGetBuffer        DSGetBuffer      = nullptr;
    F_DSRevokeBuffer     DSRevokeBuffer   = nullptr;
    F_DSClose            DSClose          = nullptr;

    // Buffer info
    using F_DSGetBufferInfo = int(*)(void*, void*, uint32_t, void*, size_t*);
    F_DSGetBufferInfo DSGetBufferInfo = nullptr;

    // helpers
    void* sym(const char* name, bool required = true);
    void check(int err, const char* msg);

    void loadSymbols();
    void openViaGC(int device_index);
    void openViaTL(int device_index);
    void openFirstDataStream();
    void announceAndQueueBuffers(size_t nBuffers, size_t bufSize);
};

} // namespace cambuffer_recorder_ng

