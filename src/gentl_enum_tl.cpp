/*
g++ gentl_enum_tl.cpp -I/opt/XIMEA/samples/_libs/GenTL -ldl -o gentl_enum_tl
export GENICAM_GENTL64_PATH="/opt/XIMEA/lib"
./gentl_enum_tl
*/

#include <iostream>
#include <dlfcn.h>
#include <cstdlib>
#include <string>

#include <GenTL_v1_5.h>

using namespace std;

static void check(GenTL::GC_ERROR err, const char* msg)
{
    if (err != GenTL::GC_ERR_SUCCESS) {
        cerr << "ERROR: " << msg << " (code=" << err << ")\n";
        exit(1);
    }
}

int main()
{
    const char* base = getenv("GENICAM_GENTL64_PATH");
    if (!base) {
        cerr << "GENICAM_GENTL64_PATH is not set!\n";
        return 1;
    }

    string cti_path = string(base) + "/ximea.gentl.cti";
    cout << "Loading CTI: " << cti_path << endl;

    void* lib = dlopen(cti_path.c_str(), RTLD_NOW);
    if (!lib) {
        cerr << "dlopen failed: " << dlerror() << endl;
        return 1;
    }

    // ---- Load core GenTL symbol for system init ----
    auto pGCInitLib  = (GenTL::PGCInitLib)  dlsym(lib, "GCInitLib");
    auto pGCCloseLib = (GenTL::PGCCloseLib) dlsym(lib, "GCCloseLib");

    if (!pGCInitLib || !pGCCloseLib) {
        cerr << "CTI missing GCInitLib or GCCloseLib" << endl;
        return 1;
    }

    // MUST call this before any TLOpen(). Required by XIMEA.
    check(pGCInitLib(), "GCInitLib");

    // ---- Load TL functions ----
    auto pTLOpen             = (GenTL::PTLOpen)             dlsym(lib, "TLOpen");
    auto pTLClose            = (GenTL::PTLClose)            dlsym(lib, "TLClose");
    auto pTLGetNumInterfaces = (GenTL::PTLGetNumInterfaces) dlsym(lib, "TLGetNumInterfaces");
    auto pTLGetInterfaceID   = (GenTL::PTLGetInterfaceID)   dlsym(lib, "TLGetInterfaceID");
    auto pTLOpenInterface    = (GenTL::PTLOpenInterface)    dlsym(lib, "TLOpenInterface");
    auto pIFGetNumDevices    = (GenTL::PIFGetNumDevices)    dlsym(lib, "IFGetNumDevices");
    auto pIFGetDeviceID      = (GenTL::PIFGetDeviceID)      dlsym(lib, "IFGetDeviceID");

    // --------------------------
    // Open System (TL)
    // --------------------------
    GenTL::TL_HANDLE hTL = nullptr;
    check(pTLOpen(&hTL), "TLOpen");

    uint32_t num_ifaces = 0;
    check(pTLGetNumInterfaces(hTL, &num_ifaces), "TLGetNumInterfaces");

    cout << "Interfaces found: " << num_ifaces << endl;

    for (uint32_t i = 0; i < num_ifaces; i++) {
        char iface_id[512] = {0};
        size_t iface_len = sizeof(iface_id);

        check(pTLGetInterfaceID(hTL, i, iface_id, &iface_len), "TLGetInterfaceID");

        cout << "  Interface[" << i << "]: " << iface_id << endl;

        GenTL::IF_HANDLE hIF = nullptr;
        check(pTLOpenInterface(hTL, iface_id, &hIF), "TLOpenInterface");

        uint32_t num_devs = 0;
        check(pIFGetNumDevices(hIF, &num_devs), "IFGetNumDevices");

        cout << "    Devices: " << num_devs << endl;

        for (uint32_t d = 0; d < num_devs; d++) {
            char dev_id[512] = {0};
            size_t dev_len = sizeof(dev_id);

            check(pIFGetDeviceID(hIF, d, dev_id, &dev_len), "IFGetDeviceID");
            cout << "      Device[" << d << "]: " << dev_id << endl;
        }
    }

    check(pTLClose(hTL), "TLClose");
    pGCCloseLib();
    dlclose(lib);

    return 0;
}


