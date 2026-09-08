#pragma once

#include <QStringList>
#include <memory>

// A snapshot owns its files independently of any recorder or worker. Registry
// access and deletion share a lock, so overlapping exports cannot unpin one
// another and a replacement recorder cannot delete a leased path.
class SegmentLease
{
public:
    SegmentLease();
    explicit SegmentLease(QStringList paths);
    ~SegmentLease();
    SegmentLease(SegmentLease&&) noexcept;
    SegmentLease& operator=(SegmentLease&&) noexcept;
    SegmentLease(const SegmentLease&) = delete;
    SegmentLease& operator=(const SegmentLease&) = delete;

    const QStringList& paths() const;
    bool isEmpty() const { return paths().isEmpty(); }
    quint64 ownerId() const;

    static bool removeIfUnleased(const QString& path);
    // Retain the newest keep paths PLUS all leases. An old lease must never
    // stop us from deleting later, unleased paths outside the normal window.
    static void trim(QStringList& chronologicalPaths, int keep);

private:
    struct State;
    std::unique_ptr<State> m_state;
};
