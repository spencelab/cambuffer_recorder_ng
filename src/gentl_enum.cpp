#include <dlfcn.h>
#include <iostream>
#include <cstring>
#include <cstdint>

// Minimal GenTL handle typedefs
typedef void* TL_HANDLE;
typedef uint32_t GC_ERROR;
#define GC_ERR_SUCCESS 0

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: gentl_enum <path/to/producer.cti>\n";
        return 1;
    }

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }

    // declare function pointer types manually
    typedef GC_ERROR (*GCInitLib_t)();
    typedef GC_ERROR (*GCGetNumSystems_t)(uint32_t*);
    typedef GC_ERROR (*GCGetSystemID_t)(uint32_t, char*, size_t*);
    typedef GC_ERROR (*GCOpenSystem_t)(const char*, TL_HANDLE*);
    typedef GC_ERROR (*SysGetNumInterfaces_t)(TL_HANDLE, uint32_t*);
    typedef GC_ERROR (*SysGetInterfaceID_t)(TL_HANDLE, uint32_t, char*, size_t*);
    typedef GC_ERROR (*SysOpenInterface_t)(TL_HANDLE, const char*, void**, void*);
    typedef GC_ERROR (*IFGetNumDevices_t)(void*, uint32_t*);
    typedef GC_ERROR (*IFGetDeviceID_t)(void*, uint32_t, char*, size_t*);

    auto GCInitLib = (GCInitLib_t)dlsym(lib, "GCInitLib");
    auto GCGetNumSystems = (GCGetNumSystems_t)dlsym(lib, "GCGetNumSystems");
    auto GCGetSystemID = (GCGetSystemID_t)dlsym(lib, "GCGetSystemID");
    auto GCOpenSystem = (GCOpenSystem_t)dlsym(lib, "GCOpenSystem");
    auto SysGetNumInterfaces = (SysGetNumInterfaces_t)dlsym(lib, "SysGetNumInterfaces");
    auto SysGetInterfaceID = (SysGetInterfaceID_t)dlsym(lib, "SysGetInterfaceID");
    auto SysOpenInterface = (SysOpenInterface_t)dlsym(lib, "SysOpenInterface");
    auto IFGetNumDevices = (IFGetNumDevices_t)dlsym(lib, "IFGetNumDevices");
    auto IFGetDeviceID = (IFGetDeviceID_t)dlsym(lib, "IFGetDeviceID");

    if (!GCInitLib) { std::cerr << "Missing GCInitLib symbol\n"; return 1; }

    GCInitLib();

    uint32_t nSys = 0;
    GCGetNumSystems(&nSys);
    std::cout << "Systems: " << nSys << std::endl;

    for (uint32_t s = 0; s < nSys; ++s) {
        char sysId[512] = {0};
        size_t sz = sizeof(sysId);
        GCGetSystemID(s, sysId, &sz);
        std::cout << "System " << s << ": " << sysId << std::endl;

        TL_HANDLE hSys = nullptr;
        GCOpenSystem(sysId, &hSys);

        uint32_t nIF = 0;
        SysGetNumInterfaces(hSys, &nIF);
        std::cout << "  Interfaces: " << nIF << std::endl;

        for (uint32_t i = 0; i < nIF; ++i) {
            char ifId[512] = {0};
            sz = sizeof(ifId);
            SysGetInterfaceID(hSys, i, ifId, &sz);
            std::cout << "    Interface " << i << ": " << ifId << std::endl;

            void* hIF = nullptr;
            SysOpenInterface(hSys, ifId, &hIF, nullptr);

            uint32_t nDev = 0;
            IFGetNumDevices(hIF, &nDev);
            std::cout << "      Devices: " << nDev << std::endl;
        }
    }

    dlclose(lib);
    return 0;
}

