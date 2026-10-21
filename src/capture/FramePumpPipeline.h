#pragma once

// Private to the FramePumpWorker translation units (FramePumpPipeline.cpp,
// ReplaySaveCoordinator.cpp). Not part of the FramePumpService API.

#include "capture/FramePumpService.h"

#include "capture/wgc_shims.h"
#include "capture/AudioCapture.h"
#include "capture/CaptureBorderState.h"
#include "capture/SegmentRecorder.h"
#include "capture/hdr/GpuToneMapper.h"

#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QVector>

// Holds every WinRT/D3D pointer plus the poll timer and fps counters for one
// active capture. Destroying it releases everything in reverse-construction
// order (see docs/replay-buffer.md).
struct FramePumpWorker::Pipeline
{
    ID3D11Device*                 d3dDevice   = nullptr;
    ID3D11DeviceContext*          d3dCtx      = nullptr;
    IDirect3DDevice*              winrtDevice = nullptr;
    IGraphicsCaptureItem*         item        = nullptr;
    IDirect3D11CaptureFramePool*  framePool   = nullptr;
    IGraphicsCaptureSession*      session     = nullptr;
    QTimer*                       readinessTimer = nullptr;
    QTimer*                       timer       = nullptr;
    SegmentRecorder*              recorder    = nullptr;   // 0.5 Step 4 H.264 segment writer
    AudioCapture*                 audio       = nullptr;    // 0.5 Step 7 WASAPI loopback
    QString                       audioFailure;             // reason matching the requested source
    QVector<float>                audioBuf;
    QString                       gameName;                // for the saved-clip Clips/ path
    QString                       executablePath;          // for sidebar game icon metadata

    // What actually happened to the Windows capture border for this session (p4-2).
    // Never assumed — derived from the OS build, the access request and the read-back.
    CaptureBorder::State          borderState  = CaptureBorder::Unknown;
    QString                       borderDetail;

    // t24 experimental HDR tone-map stage (isolated, hidden flag, see
    // ConfigKeys::InternalCaptureExperimentalHdr). hdrToneMapActive is only
    // ever true when the frame pool itself was created FP16 AND the mapper
    // initialized — the SDR path (BGRA8 pool, no mapper) is completely
    // untouched otherwise.
    capture::hdr::GpuToneMapper*  hdrToneMapper     = nullptr;
    bool                          hdrToneMapActive  = false;
    bool                          hdrToneMapWarned  = false;  // log a failed apply() once, not per-frame

    QElapsedTimer fpsClock;
    QElapsedTimer encClock;   // monotonic PTS source for the recorder
    long long encStartQpc100ns = 0;   // QPC epoch of encClock's t=0 (A/V alignment)
    int  frameCount = 0;
    UINT lastW = 0, lastH = 0;
    int  lastFmt = 0;
    bool firstFrameLogged = false;
    // The pool is created for the window's size at arm time. A game that
    // changes resolution keeps delivering frames at the old pool size, so a
    // content size that differs (and has settled) re-arms the pipeline.
    INT32 poolW = 0, poolH = 0;
    WgcSizeInt32 pendingContentSize{ 0, 0 };
    QElapsedTimer pendingContentClock;

    ~Pipeline()
    {
        if (readinessTimer) { readinessTimer->stop(); delete readinessTimer; }
        if (timer) { timer->stop(); delete timer; timer = nullptr; }
        // Finalize + release the recorder BEFORE the D3D device/context it borrows
        // (its staging texture was created from d3dDevice).
        if (recorder) { recorder->end(); delete recorder; recorder = nullptr; }
        if (audio) { audio->stop(); delete audio; audio = nullptr; }
        // Also before d3dDevice/d3dCtx — GpuToneMapper's shaders/textures were
        // created from them.
        if (hdrToneMapper) { delete hdrToneMapper; hdrToneMapper = nullptr; }
        if (session)     { session->Release();     session = nullptr; }
        if (framePool)   { framePool->Release();    framePool = nullptr; }
        if (item)        { item->Release();         item = nullptr; }
        if (winrtDevice) { winrtDevice->Release();  winrtDevice = nullptr; }
        if (d3dCtx)      { d3dCtx->Release();        d3dCtx = nullptr; }
        if (d3dDevice)   { d3dDevice->Release();     d3dDevice = nullptr; }
    }
};
