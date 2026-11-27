#include "capture/HdrCapabilities.h"
#include "capture/hdr/GpuToneMapper.h"
#include "readback_stage.inc"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using Samples = std::vector<double>;

static void require(bool ok)
{
    if (!ok) {
        std::fputs("Performance fixture failed\n", stderr);
        std::exit(1);
    }
}

static void report(const char* label, Samples all)
{
    std::sort(all.begin(), all.end());
    const auto p95 = (all.size() * 95 + 99) / 100 - 1;
    std::printf("%s (n=%zu), ms min=%.4f median=%.4f p95=%.4f max=%.4f\n",
                label, all.size(), all.front(), all[all.size() / 2], all[p95], all.back());
}

static void displayRefresh()
{
    capture::HdrReport current;
    for (int i = 0; i < 3; ++i)
        current = capture::HdrCapabilities::refreshDisplayState(current, true);
    Samples elapsed;
    for (int i = 0; i < 30; ++i) {
        QElapsedTimer time;
        time.start();
        auto fresh = capture::HdrCapabilities::refreshDisplayState(current, true);
        const bool same = capture::HdrCapabilities::sameDisplayState(current, fresh);
        elapsed.push_back(time.nsecsElapsed() / 1e6);
        require(same);
        Sleep(20);
    }
    std::printf("DXGI outputs returned: %lld\n", static_cast<long long>(current.outputs.size()));
    require(!current.outputs.isEmpty());
    report("HDR display refresh (no encoder probe)", elapsed);
}

template<class T>
static T get(ID3D11DeviceContext* context, ID3D11Query* query)
{
    T data{};
    QElapsedTimer wait;
    wait.start();
    HRESULT hr = context->GetData(query, &data, sizeof(data), 0);
    while (hr == S_FALSE && wait.elapsed() < 1000) {
        Sleep(0);
        hr = context->GetData(query, &data, sizeof(data), 0);
    }
    require(hr == S_OK);
    return data;
}

static void gpu(int width, int height)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.GetAddressOf(), nullptr, context.GetAddressOf())));

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterInfo{};
    require(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf()))
        && SUCCEEDED(adapter->GetDesc(&adapterInfo)));
    std::printf("D3D adapter: %ls (%dx%d)\n", adapterInfo.Description, width, height);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> input;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, input.GetAddressOf())));
    ComPtr<ID3D11RenderTargetView> render;
    require(SUCCEEDED(device->CreateRenderTargetView(input.Get(), nullptr, render.GetAddressOf())));
    const float colour[]{0.5f, 2.0f, 0.25f, 1.0f};
    context->ClearRenderTargetView(render.Get(), colour);

    capture::hdr::GpuToneMapper mapper;
    require(mapper.init(device.Get(), context.Get(), width, height, 200.0f));
    ComPtr<ID3D11Texture2D> staging;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())));

    const auto createQuery = [&](D3D11_QUERY type) {
        D3D11_QUERY_DESC queryDesc{type, 0};
        ComPtr<ID3D11Query> query;
        require(SUCCEEDED(device->CreateQuery(&queryDesc, query.GetAddressOf())));
        return query;
    };
    const auto disjoint = createQuery(D3D11_QUERY_TIMESTAMP_DISJOINT);
    const auto start = createQuery(D3D11_QUERY_TIMESTAMP);
    const auto afterToneMap = createQuery(D3D11_QUERY_TIMESTAMP);
    const auto afterCopy = createQuery(D3D11_QUERY_TIMESTAMP);
    Samples tone, copy, mapWall, productionReadback;

    for (int i = -8; i < 60; ++i) {
        context->Begin(disjoint.Get());
        context->End(start.Get());
        auto* output = mapper.apply(input.Get());
        require(output);
        context->End(afterToneMap.Get());
        context->CopyResource(staging.Get(), output);
        context->End(afterCopy.Get());
        context->End(disjoint.Get());
        QElapsedTimer time;
        time.start();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        const auto waitNs = time.nsecsElapsed();
        require(static_cast<const unsigned char*>(mapped.pData)[3] == 255);
        context->Unmap(staging.Get(), 0);

        const auto freq = get<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(context.Get(), disjoint.Get());
        const auto t0 = get<UINT64>(context.Get(), start.Get());
        const auto t1 = get<UINT64>(context.Get(), afterToneMap.Get());
        const auto t2 = get<UINT64>(context.Get(), afterCopy.Get());
        if (i >= 0 && !freq.Disjoint && freq.Frequency && t1 >= t0 && t2 >= t1) {
            tone.push_back(1000.0 * (t1 - t0) / freq.Frequency);
            copy.push_back(1000.0 * (t2 - t1) / freq.Frequency);
            mapWall.push_back(waitNs / 1e6);
        }
        time.restart();
        const auto image = readBgraTexture(output, device.Get(), context.Get());
        const double readbackMs = time.nsecsElapsed() / 1e6;
        require(image.width() == width && image.height() == height && qAlpha(image.pixel(0, 0)) == 255);
        if (i >= 0)
            productionReadback.push_back(readbackMs);
        Sleep(16);
    }
    require(tone.size() >= 45);
    report("GPU tone-map", tone);
    report("GPU staging copy", copy);
    report("CPU blocking Map wall time after submit", mapWall);
    report("Production screenshot readback wall time", productionReadback);
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    displayRefresh();
    gpu(1920, 1080);
    gpu(3840, 2160);
    return 0;
}
