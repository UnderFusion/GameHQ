#include "capture/SegmentLease.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace {
struct Registry {
    QMutex mutex;
    QHash<QString, int> references;
    quint64 nextOwner = 0;
};

Registry& registry()
{
    static Registry value;
    return value;
}

QString pathKey(const QString& path)
{
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}
}

struct SegmentLease::State {
    QStringList paths;
    QStringList keys;
    quint64 owner = 0;

    ~State()
    {
        auto& r = registry();
        QMutexLocker lock(&r.mutex);
        for (const QString& key : keys) {
            auto it = r.references.find(key);
            if (--it.value() == 0)
                r.references.erase(it);
        }
    }
};

SegmentLease::SegmentLease() = default;
SegmentLease::~SegmentLease() = default;
SegmentLease::SegmentLease(SegmentLease&&) noexcept = default;
SegmentLease& SegmentLease::operator=(SegmentLease&&) noexcept = default;

SegmentLease::SegmentLease(QStringList paths)
    : m_state(std::make_unique<State>())
{
    m_state->paths = std::move(paths);
    auto& r = registry();
    QMutexLocker lock(&r.mutex);
    m_state->owner = ++r.nextOwner;
    for (const QString& path : m_state->paths) {
        const QString key = pathKey(path);
        m_state->keys.append(key);
        ++r.references[key];
    }
}

const QStringList& SegmentLease::paths() const
{
    static const QStringList empty;
    return m_state ? m_state->paths : empty;
}

quint64 SegmentLease::ownerId() const
{
    return m_state ? m_state->owner : 0;
}

bool SegmentLease::removeIfUnleased(const QString& path)
{
    auto& r = registry();
    QMutexLocker lock(&r.mutex);
    if (r.references.contains(pathKey(path)))
        return false;
    return QFile::remove(path) || !QFile::exists(path);
}

void SegmentLease::trim(QStringList& paths, int keep)
{
    const qsizetype cutoff = paths.size() - qMax(0, keep);
    for (qsizetype i = cutoff; i > 0; --i) {
        if (removeIfUnleased(paths.at(i - 1)))
            paths.removeAt(i - 1);
    }
}
