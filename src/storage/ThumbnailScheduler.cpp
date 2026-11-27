#include "storage/ThumbnailScheduler.h"
#include "storage/ThumbnailService.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

QString ThumbnailScheduler::Source::key() const
{
    return type + QLatin1Char('|') + QString::number(size) + QLatin1Char('|')
        + QString::number(modified.toMSecsSinceEpoch()) + QLatin1Char('|') + path;
}

ThumbnailScheduler::Source ThumbnailScheduler::Source::stat(const QString& path,
                                                            const QString& type)
{
    Source s;
    s.path = path;
    s.type = type;
    const QFileInfo info(path);
    if (info.isFile()) {
        s.size = info.size();
        s.modified = info.lastModified().toUTC();
    }
    return s;
}

ThumbnailScheduler::ThumbnailScheduler(QString thumbnailsDir, Decoder decoder, QObject* parent)
    : QObject(parent)
    , m_thumbnailsDir(std::move(thumbnailsDir))
    , m_decoder(decoder ? std::move(decoder) : Decoder(&ThumbnailService::ensureThumbnail))
{
    m_pool.setMaxThreadCount(kMaxWorkers);
}

ThumbnailScheduler::~ThumbnailScheduler()
{
    // Jobs not yet started are dropped; started ones finish (a decode is
    // never killed) and their results are discarded with this object.
    m_closing->store(true);
    m_queue.clear();
    m_pool.waitForDone();
}

bool ThumbnailScheduler::request(int captureId, const QString& filePath, const QString& type)
{
    const Source source = Source::stat(filePath, type);
    if (!source.exists())
        return false;
    const QString key = source.key();
    if (m_failed.contains(key))
        return false;
    if (auto it = m_jobs.find(key); it != m_jobs.end()) {
        if (!it->captureIds.contains(captureId))
            it->captureIds.append(captureId);
        return true;   // shares the decode already waiting or running
    }
    if (m_jobs.size() >= kMaxJobs) {
        qWarning() << "Thumbnails: queue full, leaving" << filePath << "to the next library scan";
        return false;
    }
    m_jobs.insert(key, Job{ source, { captureId }, 0 });
    m_queue.append(key);
    startNext();
    return true;
}

void ThumbnailScheduler::startNext()
{
    while (m_active < kMaxWorkers && !m_queue.isEmpty()) {
        const QString key = m_queue.takeFirst();
        const auto it = m_jobs.constFind(key);
        if (it == m_jobs.constEnd())
            continue;
        ++m_active;
        m_peakActive = qMax(m_peakActive, m_active);
        const Source source = it->source;
        const Decoder decoder = m_decoder;
        const QString dir = m_thumbnailsDir;
        const auto closing = m_closing;
        m_pool.start([this, key, source, decoder, dir, closing] {
            if (closing->load())
                return;
            const QString thumb = decoder(source.path, source.type, dir);
            // Queued to this object's thread; dropped if it is gone by then.
            QMetaObject::invokeMethod(this, [this, key, thumb] { finished(key, thumb); },
                                      Qt::QueuedConnection);
        });
    }
}

void ThumbnailScheduler::finished(const QString& key, const QString& thumbnailPath)
{
    --m_active;
    auto it = m_jobs.find(key);
    if (it == m_jobs.end()) {
        startNext();
        return;
    }
    Job job = std::move(*it);
    m_jobs.erase(it);

    const Source now = Source::stat(job.source.path, job.source.type);
    if (now.exists() && now.key() != key) {
        // Replaced or rewritten while it was decoded: the picture may show
        // the old content but would look current. Decode the new file.
        if (!thumbnailPath.isEmpty())
            QFile::remove(thumbnailPath);
        if (job.redecodes < kMaxRedecodes && m_jobs.size() < kMaxJobs) {
            const QString nextKey = now.key();
            if (auto existing = m_jobs.find(nextKey); existing != m_jobs.end()) {
                for (int id : job.captureIds) {
                    if (!existing->captureIds.contains(id))
                        existing->captureIds.append(id);
                }
            } else {
                m_jobs.insert(nextKey, Job{ now, job.captureIds, job.redecodes + 1 });
                m_queue.append(nextKey);
            }
            startNext();
            return;
        }
        // Still changing after kMaxRedecodes: give up for now; a later scan retries.
        for (int id : job.captureIds)
            emit thumbnailFailed(id, job.source.path);
        startNext();
        return;
    }

    if (thumbnailPath.isEmpty() || !now.exists()) {
        if (now.exists())
            rememberFailure(key);
        qInfo() << "Thumbnails: no preview for" << job.source.path
                << (now.exists() ? "(undecodable; not retried until it changes)" : "(file gone)");
        for (int id : job.captureIds)
            emit thumbnailFailed(id, job.source.path);
    } else {
        for (int id : job.captureIds)
            emit thumbnailReady(id, job.source.path, thumbnailPath);
    }
    startNext();
}

void ThumbnailScheduler::rememberFailure(const QString& key)
{
    if (m_failed.size() >= kMaxRememberedFailures)
        m_failed.clear();   // bounded; a forgotten failure costs one more try
    m_failed.insert(key);
}
