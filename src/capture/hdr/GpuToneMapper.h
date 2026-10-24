#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11RasterizerState;
struct ID3D11Buffer;

namespace capture::hdr {

// GPU tone-map stage: FP16 scRGB source texture -> persistent BGRA8 output
// texture, via a shoulder-curve + sRGB full-screen-triangle pixel shader
// (identity below the knee, smooth monotonic compression above it — see
// HdrToneMapMath.h). Entirely isolated from the existing SDR capture path —
// FramePumpService only
// invokes this after the experimental HDR gate (config flag + HdrCapabilities
// + checkFp16Support) has already passed, and apply()'s output is exactly
// what SegmentRecorder already expects (BGRA8), so the recorder itself never
// changes. See t24/t22 in the plan.
//
// Not thread-safe; used only from FramePumpWorker's single MTA thread.
class GpuToneMapper
{
public:
    GpuToneMapper() = default;
    ~GpuToneMapper();

    GpuToneMapper(const GpuToneMapper&) = delete;
    GpuToneMapper& operator=(const GpuToneMapper&) = delete;

    // Compiles the shaders and allocates a persistent width x height BGRA8
    // render target. Returns false (mapper left safely destructible, no
    // partial state) on any failure — callers must fall back to the SDR path.
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, unsigned width,
              unsigned height, float sdrWhiteLevelNits);

    bool isValid() const { return m_valid; }

    // Renders fp16Src through the tone-map shader into the persistent output
    // texture and returns it — NOT a new allocation, NOT AddRef'd, same
    // lifetime as this GpuToneMapper. Returns nullptr on failure; the caller
    // should treat that frame as dropped, never attempt a pool format switch
    // mid-session.
    ID3D11Texture2D* apply(ID3D11Texture2D* fp16Src);

    // The persistent output texture, holding the last frame apply() rendered.
    // Only meaningful after at least one successful apply().
    ID3D11Texture2D* lastOutput() const { return m_valid ? m_outputTex : nullptr; }

    // True when 'device' can use R16G16B16A16_FLOAT as a sampleable 2D
    // texture. Call before init() to decide whether the experimental HDR
    // path is even worth attempting on this GPU.
    static bool checkFp16Support(ID3D11Device* device);

private:
    void releaseAll();

    ID3D11Device*             m_device      = nullptr;
    ID3D11DeviceContext*      m_context     = nullptr;
    ID3D11VertexShader*       m_vs          = nullptr;
    ID3D11PixelShader*        m_ps          = nullptr;
    ID3D11SamplerState*       m_sampler     = nullptr;
    ID3D11RasterizerState*    m_rasterizer  = nullptr;
    ID3D11Buffer*             m_params      = nullptr;
    ID3D11Texture2D*          m_outputTex   = nullptr;
    ID3D11RenderTargetView*   m_outputRtv   = nullptr;
    unsigned                  m_width       = 0;
    unsigned                  m_height      = 0;
    bool                      m_valid       = false;
};

} // namespace capture::hdr
