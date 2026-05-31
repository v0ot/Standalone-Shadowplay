// IShadowPlayApi.h — reversed from nvspapi64.dll v11.0.7.247
// Source: IDA decompile of CreateShadowPlayApiInterface_0 (RVA 0x7C730)
// and the 20-slot vtable at .rdata RVA 0x24E908.
#pragma once
#include <windows.h>
#include <cstdint>

// Magic version constants discovered in nvspapi64.dll.
// Each method takes a versioned-args struct with a leading DWORD `version` field;
// mismatch returns E_INVALIDARG (0x80070057).
namespace SpVersion {
    inline constexpr uint32_t Create                 = 0x10018; // CreateShadowPlayApiInterface input
    inline constexpr uint32_t IfaceMin               = 0x10008;
    inline constexpr uint32_t IfaceMax               = 0x20008;
    inline constexpr uint32_t Install                = 0x10014;
    inline constexpr uint32_t Uninstall              = 0x10010;
    inline constexpr uint32_t EnableDisable          = 0x10010;
    inline constexpr uint32_t Status                 = 0x10010;
    inline constexpr uint32_t Property               = 0x10060;
    inline constexpr uint32_t CreateSession          = 0x10060;
    inline constexpr uint32_t DestroySession         = 0x10010;
    inline constexpr uint32_t SessionSettings        = 0x10100;
    inline constexpr uint32_t SessionParam           = 0x10060;
    inline constexpr uint32_t SessionControlMin      = 0x10058;
    inline constexpr uint32_t SessionControlMax      = 0x50058;
    inline constexpr uint32_t RegisterCallback       = 0x10018;
    inline constexpr uint32_t UnregisterCallback     = 0x10008;
}

// Input to CreateShadowPlayApiInterface (nvspapi64 ord 17).
// VERIFIED layout from IDA decompile of CreateShadowPlayApiInterface_0 at 0x18007c730:
//   +0  DWORD version       (must be 0x10018)
//   +4  DWORD ifaceVersion  (must be in [0x10008..0x20008])
//   +8  DWORD clientId      (must be 3..10; 10 is wildcard that attaches to singleton)
//   +12 DWORD pad
//   +16 void**  ppvInterface  (output slot — function writes *ppvInterface = iface)
//
// The function checks `if ( !*(_QWORD *)(a1 + 16) )` → ppvInterface must be non-null.
// On success, it writes `**(_QWORD **)(a1 + 16) = iface;` — i.e. *args.ppvInterface = iface.
#pragma pack(push, 4)
struct SP_CREATE_INTERFACE_ARGS {
    uint32_t version;
    uint32_t ifaceVersion;
    uint32_t clientId;
    uint32_t pad;
    void**   ppvInterface;  // caller passes address of a IShadowPlayApi* slot
};

// Generic versioned-args header — every IShadowPlayApi method's first arg starts with this.
struct SP_ARGS_HDR {
    uint32_t version;
};

struct SP_ENABLE_ARGS {
    uint32_t version;       // SpVersion::EnableDisable
    uint32_t flags;
    uint32_t origin;
};

struct SP_STATUS_ARGS {
    uint32_t version;       // SpVersion::Status
    uint32_t flags;
};
#pragma pack(pop)

// CShadowPlayApi vtable. Order verified against vtable at .rdata 0x24E908.
// All methods take (this, void* versioned_args). Args structs vary by method
// and aren't fully reversed yet; treat them as opaque blobs prefixed with a
// DWORD `version` (see SpVersion::*).
using SpMethod = HRESULT (STDMETHODCALLTYPE *)(struct IShadowPlayApi*, void*);

struct IShadowPlayApi {
    struct Vtbl {
        SpMethod ReleaseInterface;          // [0]
        SpMethod OnInstall;                 // [1]
        SpMethod OnUninstall;               // [2]
        SpMethod EnableShadowPlay;          // [3]
        SpMethod DisableShadowPlay;         // [4]
        SpMethod GetStatus;                 // [5]
        SpMethod SetProperty;               // [6]
        SpMethod GetProperty;               // [7]
        SpMethod Reserved8;                 // [8]   E_NOTIMPL stub
        SpMethod Reserved9;                 // [9]   E_NOTIMPL stub
        SpMethod CreateCaptureSession;      // [10]
        SpMethod DestroyCaptureSession;     // [11]
        SpMethod SetCaptureSessionSettings; // [12]
        SpMethod GetCaptureSessionSettings; // [13]
        SpMethod CaptureSessionControl;     // [14]  start / stop / save-replay
        SpMethod GetCaptureSessionParam;    // [15]
        SpMethod SetCaptureSessionParam;    // [16]
        SpMethod RegisterCallback;          // [17]
        SpMethod UnregisterCallback;        // [18]
        SpMethod FrameCaptureControl;       // [19]  screenshot / single-frame
    };
    const Vtbl* vt;
};

// Public exports we resolve via GetProcAddress.
extern "C" {
    // ONE arg only: SP_CREATE_INTERFACE_ARGS contains the output slot at +16.
    typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateShadowPlayApiInterface)(
        SP_CREATE_INTERFACE_ARGS* args);
}
