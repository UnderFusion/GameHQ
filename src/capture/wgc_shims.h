// wgc_shims.h — Consolidated hand-written Windows.Graphics.Capture ABI shims for the
// project-local mingw-w64 toolchain (GCC 13.1, -std=c++17, MS ABI). No cppwinrt, no MSVC.
//
// This is the single coherent successor to spike/wgc/wgc_abi_shims.h. It declares every
// COM/WinRT interface needed for a DispatcherQueue-free (free-threaded, polled) WGC frame
// pump: HWND -> GraphicsCaptureItem -> free-threaded Direct3D11CaptureFramePool ->
// CaptureSession -> poll TryGetNextFrame -> Surface -> ID3D11Texture2D readback.
//
// Convention (matches the runtime-proven spike): each WinRT interface inherits
// ": public IInspectable" (which already supplies IUnknown's 3 + IInspectable's 3 slots)
// and declares ONLY its own methods, in exact MIDL vtable order. Interop interfaces inherit
// ": public IUnknown". Every slot preceding a method we actually call is declared as a stub
// so the called method lands at the correct vtable offset.
//
// IIDs/vtable order cross-verified against mingw-w64 raw headers, microsoft/windows-rs,
// tpn/winsdk-10 MIDL headers, and Microsoft Learn (3 independent derivations reconciled,
// zero contradictions). See docs/capture-engine.md.
#pragma once

#include <windows.h>
#include <unknwn.h>
#include <inspectable.h>  // IInspectable + TrustLevel (shipped by mingw-w64 — do NOT redefine)
#include <eventtoken.h>   // EventRegistrationToken
#include <dxgi.h>         // IDXGIDevice
#include <d3d11.h>        // ID3D11Texture2D, D3D11_TEXTURE2D_DESC (__uuidof available here)

// ============================================================================
//  Shared value types (WinRT structs passed by value / by pointer)
// ============================================================================

// Windows.Graphics.SizeInt32
struct WgcSizeInt32 { INT32 Width; INT32 Height; };

// Windows.Foundation.TimeSpan (100-ns ticks)
struct WgcTimeSpan  { INT64 Duration; };

// Windows.Graphics.DirectX.Direct3D11.Direct3DMultisampleDescription
struct Direct3DMultisampleDescription { INT32 Count; INT32 Quality; };

// Windows.Graphics.DirectX.Direct3D11.Direct3DSurfaceDescription
struct Direct3DSurfaceDescription {
    INT32 Width;
    INT32 Height;
    INT32 Format;  // DirectXPixelFormat (INT32 in the ABI)
    Direct3DMultisampleDescription MultisampleDescription;
};

// Windows.Graphics.DirectX.DirectXPixelFormat (INT32 in ABI; maps 1:1 to DXGI_FORMAT)
enum DirectXPixelFormat : INT32 {
    DirectXPixelFormat_R16G16B16A16Float      = 10,  // DXGI_FORMAT_R16G16B16A16_FLOAT (HDR/scRGB)
    DirectXPixelFormat_B8G8R8A8UIntNormalized = 87,  // DXGI_FORMAT_B8G8R8A8_UNORM (standard BGRA)
};

// ============================================================================
//  Forward declarations
// ============================================================================
struct IDirect3DDevice;
struct IDirect3DSurface;
struct IGraphicsCaptureItem;
struct IGraphicsCaptureSession;
struct IDirect3D11CaptureFrame;
struct IDirect3D11CaptureFramePool;
struct IDispatcherQueue;  // Windows.System.IDispatcherQueue — opaque, only returned by pointer

// ============================================================================
//  Direct3D bridging interfaces
// ============================================================================

// Windows.Graphics.DirectX.Direct3D11.IDirect3DDevice : IInspectable
//   {A37624AB-8D5F-4650-9D3E-9EAE3D9BC670}
// Obtained by QI'ing the IInspectable from CreateDirect3D11DeviceFromDXGIDevice.
// Sole own method: Trim() (a command, NOT a get/put pair) at slot 6.
struct __declspec(uuid("A37624AB-8D5F-4650-9D3E-9EAE3D9BC670"))
IDirect3DDevice : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Trim() = 0;                                   // slot 6
};

// Windows.Graphics.DirectX.Direct3D11.IDirect3DSurface : IInspectable
//   {0BF4A146-13C1-4694-BEE3-7ABF15EAF586}
// Returned by IDirect3D11CaptureFrame::get_Surface. Sole own method: get_Description
// (slot 6). For texture readback you QI this to IDirect3DDxgiInterfaceAccess instead.
struct __declspec(uuid("0BF4A146-13C1-4694-BEE3-7ABF15EAF586"))
IDirect3DSurface : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Description(Direct3DSurfaceDescription* value) = 0; // slot 6
};

// Windows.Graphics.DirectX.Direct3D11.IDirect3DDxgiInterfaceAccess : IUnknown
//   {A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1}
// QI a IDirect3DSurface/IDirect3DDevice to this, then GetInterface(__uuidof(ID3D11Texture2D),
// (void**)&tex) yields the raw D3D11 texture. Sole own method: GetInterface at slot 3.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** object) = 0;  // slot 3
};

// ============================================================================
//  Capture item + interop bridge
// ============================================================================

// Windows.Graphics.Capture.IGraphicsCaptureItem : IInspectable
//   {79C3F95B-31F7-4EC2-A464-632EF5D30760}
struct __declspec(uuid("79C3F95B-31F7-4EC2-A464-632EF5D30760"))
IGraphicsCaptureItem : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING* value) = 0;          // slot 6
    virtual HRESULT STDMETHODCALLTYPE get_Size(WgcSizeInt32* value) = 0;            // slot 7
    // handler is really ITypedEventHandler<GraphicsCaptureItem*, IInspectable*>* —
    // ABI-identical to IUnknown* as a raw pointer.
    virtual HRESULT STDMETHODCALLTYPE add_Closed(IUnknown* handler,
                                                 EventRegistrationToken* token) = 0; // slot 8
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventRegistrationToken token) = 0; // slot 9
};

// Windows.Graphics.Capture.IGraphicsCaptureItemInterop : IUnknown
//   {3628E81B-3CAC-4C60-B7F4-23CE0E0C3356}
// Win32<->WinRT bridge: turns an HWND/HMONITOR into a GraphicsCaptureItem.
// Activate via RoGetActivationFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem",
//                                     IID_IGraphicsCaptureItemInterop, ...).
struct __declspec(uuid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356"))
IGraphicsCaptureItemInterop : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void** result) = 0;   // slot 3
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void** result) = 0; // slot 4
};

// ============================================================================
//  Capture frame
// ============================================================================

// Windows.Graphics.Capture.IDirect3D11CaptureFrame : IInspectable
//   {FA50C623-38DA-4B32-ACF3-FA9734AD800E}
struct __declspec(uuid("FA50C623-38DA-4B32-ACF3-FA9734AD800E"))
IDirect3D11CaptureFrame : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IDirect3DSurface** value) = 0;    // slot 6
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(WgcTimeSpan* value) = 0; // slot 7 (stub)
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(WgcSizeInt32* value) = 0;     // slot 8
};

// ============================================================================
//  Frame pool + activation factories
// ============================================================================

// Windows.Graphics.Capture.IDirect3D11CaptureFramePool : IInspectable
//   {24EB6D22-1975-422E-82E7-780DBD8DDF24}
// Full ABI. We call TryGetNextFrame (slot 7) and CreateCaptureSession (slot 10); the
// FrameArrived slots (8/9) are stubbed to keep CreateCaptureSession at the right offset.
struct __declspec(uuid("24EB6D22-1975-422E-82E7-780DBD8DDF24"))
IDirect3D11CaptureFramePool : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IDirect3DDevice* device, INT32 pixelFormat,
                                               INT32 numberOfBuffers, WgcSizeInt32 size) = 0;   // slot 6 (stub)
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame** result) = 0;    // slot 7
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(IUnknown* handler,
                                                       EventRegistrationToken* token) = 0;      // slot 8 (stub)
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventRegistrationToken token) = 0;    // slot 9 (stub)
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem* item,
                                                           IGraphicsCaptureSession** result) = 0; // slot 10
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IDispatcherQueue** value) = 0;        // slot 11 (stub)
};

// Windows.Graphics.Capture.IDirect3D11CaptureFramePoolStatics : IInspectable
//   {7784056A-67AA-4D53-AE54-1088D5A8CA21}   (DispatcherQueue-bound factory)
struct __declspec(uuid("7784056A-67AA-4D53-AE54-1088D5A8CA21"))
IDirect3D11CaptureFramePoolStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Create(IDirect3DDevice* device, INT32 pixelFormat,
                                             INT32 numberOfBuffers, WgcSizeInt32 size,
                                             IDirect3D11CaptureFramePool** result) = 0;         // slot 6
};

// Windows.Graphics.Capture.IDirect3D11CaptureFramePoolStatics2 : IInspectable
//   {589B103F-6BBC-5DF5-A991-02E28B3B66D5}   (free-threaded factory — no DispatcherQueue)
// This is the poll-pump path. Param order is IDENTICAL to Create.
struct __declspec(uuid("589B103F-6BBC-5DF5-A991-02E28B3B66D5"))
IDirect3D11CaptureFramePoolStatics2 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IDirect3DDevice* device, INT32 pixelFormat,
                                                         INT32 numberOfBuffers, WgcSizeInt32 size,
                                                         IDirect3D11CaptureFramePool** result) = 0; // slot 6
};

// ============================================================================
//  Capture session
// ============================================================================

// Windows.Graphics.Capture.IGraphicsCaptureSession : IInspectable
//   {814E42A9-F70F-4AD7-939B-FDDCC6EB880D}
// StartCapture is the ENTIRE base vtable (slot 6). Property accessors live on
// IGraphicsCaptureSession2/3/... (queried separately, not declared here).
struct __declspec(uuid("814E42A9-F70F-4AD7-939B-FDDCC6EB880D"))
IGraphicsCaptureSession : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;                           // slot 6
};

// Windows.Graphics.Capture.IGraphicsCaptureSession3 : IInspectable
//   {F2CDD966-22AE-5EA1-9596-3A289344C3BE}  — get/put IsBorderRequired (Win11).
// NOT declared by mingw's headers (only forward-declared); this is the public SDK
// IID. If it's wrong or the OS lacks the capability, QueryInterface fails gracefully
// (E_NOINTERFACE) and the yellow capture border simply stays — no crash.
struct __declspec(uuid("F2CDD966-22AE-5EA1-9596-3A289344C3BE"))
IGraphicsCaptureSession3 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(unsigned char* value) = 0; // slot 6
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(unsigned char value) = 0;  // slot 7
};

// ============================================================================
//  Borderless capture access (Win11) — p4-2
// ============================================================================
// The yellow capture border can only be suppressed by a process that holds
// Borderless capture access. mingw-w64 ships no declaration for any of this, and
// the statics IID is not in any public mingw header either — it was read off the
// live activation factory via IInspectable::GetIids() by the p4-1 spike
// (tools/spikes/wgc_border_probe.cpp) rather than guessed.

// Windows.Graphics.Capture.GraphicsCaptureAccessKind
// Values read out of Windows.Graphics.winmd by tools/spikes/winrt_enum_probe.exe.
enum GraphicsCaptureAccessKind : INT32 {
    GraphicsCaptureAccessKind_Borderless   = 0,
    GraphicsCaptureAccessKind_Programmatic = 1,
};

// Windows.Foundation.IAsyncOperation<AppCapabilityAccessStatus>.
// Parameterized interfaces have computed IIDs, so this is never QI'd — we only use
// the pointer RequestAccessAsync returns, at its own vtable offsets.
struct IAsyncOperationAppCapabilityAccessStatus : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;   // slot 6 (stub)
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;  // slot 7 (stub)
    virtual HRESULT STDMETHODCALLTYPE GetResults(INT32* results) = 0;         // slot 8
};

// Windows.Graphics.Capture.IGraphicsCaptureAccessStatics : IInspectable
//   {743ED370-06EC-5040-A58A-901F0F757095}
struct __declspec(uuid("743ED370-06EC-5040-A58A-901F0F757095"))
IGraphicsCaptureAccessStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(
        INT32 kind, IAsyncOperationAppCapabilityAccessStatus** operation) = 0; // slot 6
};

// Windows.Foundation.IAsyncInfo : IInspectable  {00000036-0000-0000-C000-000000000046}
// Used to wait for the access request without a Completed handler.
struct __declspec(uuid("00000036-0000-0000-C000-000000000046"))
IAsyncInfoShim : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;                 // slot 6 (stub)
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32* status) = 0;          // slot 7
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;       // slot 8
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;                           // slot 9 (stub)
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;                            // slot 10
};

// Windows.Foundation.AsyncStatus
enum WgcAsyncStatus : INT32 {
    WgcAsyncStatus_Started   = 0,
    WgcAsyncStatus_Completed = 1,
    WgcAsyncStatus_Canceled  = 2,
    WgcAsyncStatus_Error     = 3,
};

// ============================================================================
//  IID constants (C initializer form — explicit, not reliant on __uuidof under MinGW)
// ============================================================================
static const GUID IID_IDirect3DDevice =
    { 0xA37624AB, 0x8D5F, 0x4650, { 0x9D, 0x3E, 0x9E, 0xAE, 0x3D, 0x9B, 0xC6, 0x70 } };
static const GUID IID_IDirect3DSurface =
    { 0x0BF4A146, 0x13C1, 0x4694, { 0xBE, 0xE3, 0x7A, 0xBF, 0x15, 0xEA, 0xF5, 0x86 } };
static const GUID IID_IDirect3DDxgiInterfaceAccess =
    { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };
static const GUID IID_IGraphicsCaptureItem =
    { 0x79C3F95B, 0x31F7, 0x4EC2, { 0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60 } };
static const GUID IID_IGraphicsCaptureItemInterop =
    { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };
static const GUID IID_IDirect3D11CaptureFrame =
    { 0xFA50C623, 0x38DA, 0x4B32, { 0xAC, 0xF3, 0xFA, 0x97, 0x34, 0xAD, 0x80, 0x0E } };
static const GUID IID_IDirect3D11CaptureFramePool =
    { 0x24EB6D22, 0x1975, 0x422E, { 0x82, 0xE7, 0x78, 0x0D, 0xBD, 0x8D, 0xDF, 0x24 } };
static const GUID IID_IDirect3D11CaptureFramePoolStatics =
    { 0x7784056A, 0x67AA, 0x4D53, { 0xAE, 0x54, 0x10, 0x88, 0xD5, 0xA8, 0xCA, 0x21 } };
static const GUID IID_IDirect3D11CaptureFramePoolStatics2 =
    { 0x589B103F, 0x6BBC, 0x5DF5, { 0xA9, 0x91, 0x02, 0xE2, 0x8B, 0x3B, 0x66, 0xD5 } };
static const GUID IID_IGraphicsCaptureSession =
    { 0x814E42A9, 0xF70F, 0x4AD7, { 0x93, 0x9B, 0xFD, 0xDC, 0xC6, 0xEB, 0x88, 0x0D } };
static const GUID IID_IGraphicsCaptureSession3 =
    { 0xF2CDD966, 0x22AE, 0x5EA1, { 0x95, 0x96, 0x3A, 0x28, 0x93, 0x44, 0xC3, 0xBE } };
static const GUID IID_IGraphicsCaptureAccessStatics =
    { 0x743ED370, 0x06EC, 0x5040, { 0xA5, 0x8A, 0x90, 0x1F, 0x0F, 0x75, 0x70, 0x95 } };
static const GUID IID_IAsyncInfoShim =
    { 0x00000036, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

// ============================================================================
//  Activation runtimeclass strings
// ============================================================================
static const wchar_t* const kWgcCaptureItemClass =
    L"Windows.Graphics.Capture.GraphicsCaptureItem";                 // -> IGraphicsCaptureItemInterop
static const wchar_t* const kWgcFramePoolClass =
    L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";          // -> ...FramePoolStatics / Statics2
static const wchar_t* const kWgcCaptureAccessClass =
    L"Windows.Graphics.Capture.GraphicsCaptureAccess";               // -> IGraphicsCaptureAccessStatics

// ============================================================================
//  d3d11.dll export — resolved at runtime via GetProcAddress (as the spike does)
// ============================================================================
typedef HRESULT (WINAPI *PFN_CreateDirect3D11DeviceFromDXGIDevice)(
    IDXGIDevice* dxgiDevice, IInspectable** graphicsDevice);
