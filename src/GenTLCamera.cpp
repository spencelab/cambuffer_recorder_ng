#include "cambuffer_recorder_ng/GenTLCamera.hpp"
#include <dlfcn.h>
#include <iostream>
#include <chrono>

namespace cambuffer_recorder_ng {

static constexpr int GC_ERR_SUCCESS = 0;

// ---------------- helpers ----------------
void* GenTLCamera::sym(const char* name, bool required) {
    void* p = dlsym(lib_, name);
    if (!p && required) {
        throw std::runtime_error(std::string("GenTL symbol missing: ") + name);
    }
    return p;
}
void GenTLCamera::check(int err, const char* msg) {
    if (err != GC_ERR_SUCCESS)
        throw std::runtime_error(std::string(msg) + " (GenTL err " + std::to_string(err) + ")");
}

// --------------- open/close ---------------
void GenTLCamera::open(const std::string& cti_path, int device_index)
{
    if (lib_) close();

    lib_ = dlopen(cti_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib_) throw std::runtime_error("dlopen failed for " + cti_path);

    loadSymbols();

    // Try GC path first; if GCGetNumSystems is missing, use TL path.
    use_gc_path_ = (GCGetNumSystems != nullptr);
    if (use_gc_path_) openViaGC(device_index);
    else              openViaTL(device_index);

    openFirstDataStream();

    // Pre-announce and queue a few buffers (2 MB each to start).
    announceAndQueueBuffers(4, 2 * 1024 * 1024);
}

void GenTLCamera::close()
{
    stop();

    for (auto& b : bufs_) {
        if (b.hBuf) { DSRevokeBuffer(hDS_, b.hBuf, nullptr, nullptr); b.hBuf = nullptr; }
        if (b.pData){ ::operator delete(b.pData); b.pData = nullptr; }
    }
    bufs_.clear();

    if (hDS_)   { DSClose(hDS_); hDS_ = nullptr; }
    if (hDev_)  { DevClose(hDev_); hDev_ = nullptr; }
    if (hIF_)   {
        if (use_gc_path_) { SysCloseInterface(hIF_); }
        else { IFClose(hIF_); } // on TL path, some producers close IF via IFClose; XIMEA exports IFClose
        hIF_ = nullptr;
    }
    if (hSystem_) { GCCloseSystem(hSystem_); hSystem_ = nullptr; }
    if (hTL_)     { if (TLClose) TLClose(hTL_); hTL_ = nullptr; }

    if (GCCloseLib) GCCloseLib();
    if (lib_) { dlclose(lib_); lib_ = nullptr; }
}

void GenTLCamera::start()
{
    if (running_) return;
    check(DSStartAcquisition(hDS_, 0xffffffffffffffffULL, 0), "DSStartAcquisition");
    running_ = true;
}

void GenTLCamera::stop()
{
    if (!running_) return;
    DSStopAcquisition(hDS_, 0);
    running_ = false;
}

// --------------- grab ---------------
bool GenTLCamera::grab(uint8_t*& data, size_t& size, uint64_t& ts,
                       int& width, int& height, int& stride, int timeout_ms)
{
    if (!running_) return false;

    void* hBuf = nullptr;
    int err = DSGetBuffer(hDS_, &hBuf, static_cast<uint32_t>(timeout_ms));
    if (err != GC_ERR_SUCCESS || !hBuf) return false;

    // Common BUFFER_INFO_* numeric codes used by many producers:
    //  0x0001: BASE pointer
    //  0x0002: SIZE
    //  0x0005: TIMESTAMP (ns)
    //  0x0011: WIDTH
    //  0x0012: HEIGHT
    //  0x0013: LINE_SIZE (stride)
    void* ptr = nullptr; size_t sz = 0;
    check(DSGetBufferInfo(hDS_, hBuf, 0x0001, &ptr, &sz), "DSGetBufferInfo ptr");
    data = static_cast<uint8_t*>(ptr);
    size = sz;

    uint64_t ts_ns = 0; size_t ts_sz = sizeof(ts_ns);
    DSGetBufferInfo(hDS_, hBuf, 0x0005, &ts_ns, &ts_sz);
    ts = ts_ns;

    uint64_t w64=0, h64=0, ls64=0; size_t n8=sizeof(uint64_t);
    DSGetBufferInfo(hDS_, hBuf, 0x0011, &w64, &n8);
    DSGetBufferInfo(hDS_, hBuf, 0x0012, &h64, &n8);
    DSGetBufferInfo(hDS_, hBuf, 0x0013, &ls64, &n8);
    width  = static_cast<int>(w64);
    height = static_cast<int>(h64);
    stride = static_cast<int>(ls64);

    check(DSQueueBuffer(hDS_, hBuf), "DSQueueBuffer");
    return true;
}

// --------------- internals ---------------
void GenTLCamera::loadSymbols()
{
    // Library
    GCInitLib   = reinterpret_cast<F_GCInitLib>(sym("GCInitLib", false));
    GCCloseLib  = reinterpret_cast<F_GCCloseLib>(sym("GCCloseLib", false));
    if (GCInitLib) check(GCInitLib(), "GCInitLib");

    // GC path symbols (may be absent)
    GCGetNumSystems    = reinterpret_cast<F_GCGetNumSystems>(sym("GCGetNumSystems", false));
    GCGetSystemID      = reinterpret_cast<F_GCGetSystemID>(sym("GCGetSystemID",   false));
    GCOpenSystem       = reinterpret_cast<F_GCOpenSystem>(sym("GCOpenSystem",     false));
    GCCloseSystem      = reinterpret_cast<F_GCCloseSystem>(sym("GCCloseSystem",   false));
    SysGetNumInterfaces= reinterpret_cast<F_SysGetNumInterfaces>(sym("SysGetNumInterfaces", false));
    SysGetInterfaceID  = reinterpret_cast<F_SysGetInterfaceID>(sym("SysGetInterfaceID",   false));
    SysOpenInterface   = reinterpret_cast<F_SysOpenInterface>(sym("SysOpenInterface",    false));
    SysCloseInterface  = reinterpret_cast<F_SysCloseInterface>(sym("SysCloseInterface",   false));

    // TL path symbols (XIMEA)
    TLOpen             = reinterpret_cast<F_TLOpen>(sym("TLOpen", false));
    TLClose            = reinterpret_cast<F_TLClose>(sym("TLClose", false));
    TLGetNumInterfaces = reinterpret_cast<F_TLGetNumInterfaces>(sym("TLGetNumInterfaces", false));
    TLGetInterfaceID   = reinterpret_cast<F_TLGetInterfaceID>(sym("TLGetInterfaceID", false));

    // Common lower levels
    IFGetNumDevices = reinterpret_cast<F_IFGetNumDevices>(sym("IFGetNumDevices"));
    IFGetDeviceID   = reinterpret_cast<F_IFGetDeviceID>(sym("IFGetDeviceID"));
    IFOpenDevice    = reinterpret_cast<F_IFOpenDevice>(sym("IFOpenDevice"));
    IFClose         = reinterpret_cast<F_IFClose>(sym("IFClose"));

    DevGetNumDataStreams = reinterpret_cast<F_DevGetNumDataStreams>(sym("DevGetNumDataStreams"));
    DevGetDataStreamID   = reinterpret_cast<F_DevGetDataStreamID>(sym("DevGetDataStreamID"));
    DevOpenDataStream    = reinterpret_cast<F_DevOpenDataStream>(sym("DevOpenDataStream"));
    DevClose             = reinterpret_cast<F_DevClose>(sym("DevClose"));

    // DataStream
    DSAnnounceBuffer     = reinterpret_cast<F_DSAnnounceBuffer>(sym("DSAnnounceBuffer"));
    DSQueueBuffer        = reinterpret_cast<F_DSQueueBuffer>(sym("DSQueueBuffer"));
    DSStartAcquisition   = reinterpret_cast<F_DSStartAcquisition>(sym("DSStartAcquisition"));
    DSStopAcquisition    = reinterpret_cast<F_DSStopAcquisition>(sym("DSStopAcquisition"));

    // Some producers (Spinnaker) export DSGetBuffer; XIMEA uses DSGetBufferChunkData
    DSGetBuffer = reinterpret_cast<F_DSGetBuffer>(dlsym(lib_, "DSGetBuffer"));
    if (!DSGetBuffer) {
        DSGetBuffer = reinterpret_cast<F_DSGetBuffer>(dlsym(lib_, "DSGetBufferChunkData"));
        if (DSGetBuffer)
            std::cerr << "[GenTLCamera] Using DSGetBufferChunkData (XIMEA path)\n";
        else
            throw std::runtime_error("GenTL producer lacks both DSGetBuffer and DSGetBufferChunkData");
    }

    DSRevokeBuffer       = reinterpret_cast<F_DSRevokeBuffer>(sym("DSRevokeBuffer"));
    DSClose              = reinterpret_cast<F_DSClose>(sym("DSClose"));
    DSGetBufferInfo      = reinterpret_cast<F_DSGetBufferInfo>(sym("DSGetBufferInfo"));
}

void GenTLCamera::openViaGC(int device_index)
{
    uint32_t nSys = 0;
    check(GCGetNumSystems(&nSys), "GCGetNumSystems");
    if (nSys == 0) throw std::runtime_error("No GenTL systems found");

    char sysID[512] = {0}; size_t sz = sizeof(sysID);
    check(GCGetSystemID(0, sysID, &sz), "GCGetSystemID");
    check(GCOpenSystem(sysID, &hSystem_), "GCOpenSystem");

    uint32_t nIF = 0;
    check(SysGetNumInterfaces(hSystem_, &nIF), "SysGetNumInterfaces");
    if (nIF == 0) throw std::runtime_error("No interfaces in system");

    char ifID[512] = {0}; sz = sizeof(ifID);
    check(SysGetInterfaceID(hSystem_, 0, ifID, &sz), "SysGetInterfaceID");
    check(SysOpenInterface(hSystem_, ifID, &hIF_, nullptr), "SysOpenInterface");

    uint32_t nDev = 0;
    check(IFGetNumDevices(hIF_, &nDev), "IFGetNumDevices");
    if (nDev == 0) throw std::runtime_error("No devices on interface");

    int idx = (device_index >= 0 && device_index < static_cast<int>(nDev)) ? device_index : 0;
    char devID[512] = {0}; sz = sizeof(devID);
    check(IFGetDeviceID(hIF_, static_cast<uint32_t>(idx), devID, &sz), "IFGetDeviceID");
    check(IFOpenDevice(hIF_, devID, &hDev_, /*open flags*/ 1), "IFOpenDevice");
}

void GenTLCamera::openViaTL(int device_index)
{
    if (!TLOpen || !TLGetNumInterfaces || !TLGetInterfaceID)
        throw std::runtime_error("Producer lacks TL-level entry points");

    check(TLOpen(&hTL_), "TLOpen");

    uint32_t nIF = 0;
    check(TLGetNumInterfaces(hTL_, &nIF), "TLGetNumInterfaces");
    if (nIF == 0) throw std::runtime_error("No interfaces via TL");

    char ifID[512] = {0}; size_t sz = sizeof(ifID);
    check(TLGetInterfaceID(hTL_, 0, ifID, &sz), "TLGetInterfaceID");

    // XIMEA exposes TLOpenInterface (returns IF handle), but we already loaded IF* symbols for device ops
    using F_TLOpenInterface = int(*)(void*, const char*, void**);
    auto TLOpenInterface = reinterpret_cast<F_TLOpenInterface>(sym("TLOpenInterface"));
    check(TLOpenInterface(hTL_, ifID, &hIF_), "TLOpenInterface");

    uint32_t nDev = 0;
    check(IFGetNumDevices(hIF_, &nDev), "IFGetNumDevices");
    if (nDev == 0) throw std::runtime_error("No devices on interface");

    int idx = (device_index >= 0 && device_index < static_cast<int>(nDev)) ? device_index : 0;
    char devID[512] = {0}; sz = sizeof(devID);
    check(IFGetDeviceID(hIF_, static_cast<uint32_t>(idx), devID, &sz), "IFGetDeviceID");
    check(IFOpenDevice(hIF_, devID, &hDev_, /*open flags*/ 1), "IFOpenDevice");
}

void GenTLCamera::openFirstDataStream()
{
    uint32_t nDS = 0;
    check(DevGetNumDataStreams(hDev_, &nDS), "DevGetNumDataStreams");
    if (nDS == 0) throw std::runtime_error("Device has no datastreams");

    char dsID[512] = {0}; size_t sz = sizeof(dsID);
    check(DevGetDataStreamID(hDev_, 0, dsID, &sz), "DevGetDataStreamID");
    check(DevOpenDataStream(hDev_, dsID, &hDS_), "DevOpenDataStream");
}

void GenTLCamera::announceAndQueueBuffers(size_t nBuffers, size_t bufSize)
{
    bufs_.resize(nBuffers);
    for (auto& b : bufs_) {
        b.pData = ::operator new(bufSize);
        b.sz = bufSize;
        check(DSAnnounceBuffer(hDS_, b.pData, b.sz, &b.hBuf), "DSAnnounceBuffer");
        check(DSQueueBuffer(hDS_, b.hBuf), "DSQueueBuffer");
    }
}

} // namespace cambuffer_recorder_ng

