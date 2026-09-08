#pragma once

#include "capture/SegmentLease.h"

#include <QDebug>
#include <QThread>
#include <functional>
#include <utility>

// Owned and joined by FramePumpWorker. Work must capture values/shared results,
// never a worker, pipeline or recorder. No event loop or finished callback is
// needed to release the lease, even when the receiver has already disappeared.
class ReplayExportTask final : public QThread
{
public:
    using Work = std::function<void(const QStringList&)>;

    ReplayExportTask(SegmentLease lease, Work work)
        : m_lease(std::move(lease)), m_work(std::move(work)) {}
    ~ReplayExportTask() override { finish(); }

    // A slow muxer is not cancellable. After the normal grace period, keep
    // joining rather than destroy a running QThread or tear down its process.
    bool finish(unsigned long graceMs = 5000)
    {
        if (wait(graceMs))
            return true;
        qWarning() << "ReplayExportTask: shutdown grace period elapsed; waiting for export";
        wait();
        return false;
    }

protected:
    void run() override
    {
        // Local ownership is essential: QThread/callback destruction may occur
        // much later than run() returning, or without processing queued events.
        SegmentLease lease = std::move(m_lease);
        try {
            m_work(lease.paths());
        } catch (...) {
            qWarning() << "ReplayExportTask: export failed with an exception";
        }
    }

private:
    SegmentLease m_lease;
    Work m_work;
};
