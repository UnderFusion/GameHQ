#include "capture/AudioCapture.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <windows.h>

namespace
{
struct AllocationCount {
    size_t calls;
    size_t bytes;
};

thread_local AllocationCount* allocationCount = nullptr;

void require(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
}

// Counts ordinary C++ allocations made by the fixture executable. Qt, the
// driver, and allocator calls that bypass operator new are outside this scope.
void* operator new(size_t size)
{
    if (allocationCount) {
        ++allocationCount->calls;
        allocationCount->bytes += size;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }

namespace
{
constexpr int sampleRate = 48000;
constexpr int channels = 2;
constexpr int packetFrames = 480;
constexpr int packetCount = 10000;
constexpr long long packetTicks = 100000;
using Packet = std::array<float, packetFrames * channels>;

void sampleFifo()
{
    AllocationCount pushAllocations{};
    AllocationCount popAllocations{};
    Packet input{};
    Packet output{};
    input.fill(0.25f);
    AudioCapture fifo;
    fifo.m_sampleRate = sampleRate;
    fifo.m_channels = channels;

    std::vector<qint64> serviceNs;
    serviceNs.reserve(packetCount);
    QElapsedTimer clock;
    for (int i = 0; i < packetCount; ++i) {
        const auto timestamp = i * packetTicks;
        clock.start();
        allocationCount = &pushAllocations;
        fifo.pushFifo(input.data(), packetFrames, timestamp);
        allocationCount = &popAllocations;
        long long resultTime = 0;
        const auto frames = fifo.popFifo(output.data(), packetFrames, &resultTime);
        allocationCount = nullptr;
        serviceNs.push_back(clock.nsecsElapsed());
        require(frames == packetFrames && resultTime == timestamp && input == output,
                "FIFO round trip lost samples or timestamp");
    }

    std::sort(serviceNs.begin(), serviceNs.end());
    std::printf("FIFO %d packets: push %zu allocs / %zu B, pop %zu allocs / %zu B,"
                " push+pop p50 %.3f us, p95 %.3f us, p99 %.3f us\n",
                packetCount, pushAllocations.calls, pushAllocations.bytes,
                popAllocations.calls, popAllocations.bytes, serviceNs[packetCount / 2] / 1000.0,
                serviceNs[packetCount * 95 / 100 - 1] / 1000.0,
                serviceNs[packetCount * 99 / 100 - 1] / 1000.0);

    constexpr int burstPackets = 310;
    constexpr int retainedPackets = 2 * sampleRate / packetFrames;
    for (int i = 0; i < burstPackets; ++i)
        fifo.pushFifo(input.data(), packetFrames, i * packetTicks);
    require(fifo.m_fifoFrames == 2 * sampleRate
                && fifo.m_fifoDroppedFrames == (burstPackets - retainedPackets) * packetFrames,
            "FIFO capacity or eviction count is wrong");

    long long timestamp = 0;
    require(fifo.popFifo(output.data(), packetFrames / 2, &timestamp) == packetFrames / 2
                && timestamp == (burstPackets - retainedPackets) * packetTicks,
            "FIFO did not evict the oldest packets");
    require(fifo.popFifo(output.data(), packetFrames / 2, &timestamp) == packetFrames / 2
                && timestamp == (burstPackets - retainedPackets) * packetTicks + packetTicks / 2,
            "Partial FIFO read did not advance the timestamp");
    std::printf("FIFO burst: retained %d of %d packets, dropped %d oldest; partial timestamp verified\n",
                retainedPackets, burstPackets, burstPackets - retainedPackets);
}

quint64 threadTime()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user), "GetThreadTimes failed");
    const auto ticks = [](const FILETIME& time) {
        return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}

void sampleTimers(bool enabled)
{
    QEventLoop loop;
    QElapsedTimer time;
    QTimer capture, overlay, hdr, foreground;
    std::array<int, 4> callbacks{};
    const auto start = [&](QTimer& timer, int ms, int index) {
        QObject::connect(&timer, &QTimer::timeout, &loop, [&, index] { ++callbacks[index]; });
        timer.start(ms);
    };
    capture.setTimerType(Qt::PreciseTimer);
    if (enabled) {
        start(capture, 16, 0);
        start(overlay, 250, 1);
        start(hdr, 1000, 2);
        start(foreground, 1500, 3);
    }
    const auto before = threadTime();
    time.start();
    QTimer::singleShot(4800, Qt::PreciseTimer, &loop, &QEventLoop::quit);
    loop.exec();
    std::printf("Timers %s: wall %.3f ms, thread CPU %.3f ms, callbacks 16/250/1000/1500 ms = %d/%d/%d/%d\n",
                enabled ? "on" : "off", time.nsecsElapsed() / 1e6, (threadTime() - before) / 1e4,
                callbacks[0], callbacks[1], callbacks[2], callbacks[3]);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && std::strcmp(argv[1], "fifo") == 0) {
        sampleFifo();
    } else if (argc == 2 && std::strcmp(argv[1], "timers-on") == 0) {
        sampleTimers(true);
    } else if (argc == 2 && std::strcmp(argv[1], "timers-off") == 0) {
        sampleTimers(false);
    } else {
        std::fputs("Usage: fifo | timers-on | timers-off\n", stderr);
        return 2;
    }
    return 0;
}
