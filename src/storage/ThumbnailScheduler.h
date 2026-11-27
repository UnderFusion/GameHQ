#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

// Generates thumbnails off the GUI thread for captures that are already in
// the library. A capture is committed and indexed first; its preview follows
// when ready, so a slow or failing decode never delays or fails the capture.
//
// Bounded: at most kMaxWorkers decodes run, at most kMaxJobs distinct sources
// wait or run, and each source (path, type, size, modification time) is
// decoded once however often it is requested. A source that fails to decode
// is not retried until it changes. A source that changes while it is decoded
// is decoded again instead of publishing the old picture.
//
// Lives on the GUI thread. Results come back there; nothing reaches a
// destroyed scheduler. Destruction drops waiting jobs and waits for running
// ones: a decode is never abandoned or killed.
class ThumbnailScheduler : public QObject
{
    Q_OBJECT
public:
    // Produces (or finds cached) the thumbnail and returns its path, "" on
    // failure. Runs on a pool thread. Defaults to ThumbnailService.
    using Decoder = std::function<QString(const QString& filePath, const QString& type,
                                          const QString& thumbnailsDir)>;

    explicit ThumbnailScheduler(QString thumbnailsDir, Decoder decoder = {},
                                QObject* parent = nullptr);
    ~ThumbnailScheduler() override;

    static constexpr int kMaxWorkers = 2;
    static constexpr int kMaxJobs = 128;
    static constexpr int kMaxRememberedFailures = 512;
    static constexpr int kMaxRedecodes = 2;

    // False when the source is missing, known to fail unchanged, or the queue
    // is full; the next library scan repairs thumbnails that never arrived.
    bool request(int captureId, const QString& filePath, const QString& type);

    int queuedJobs() const { return int(m_queue.size()); }
    int activeJobs() const { return m_active; }
    int peakActiveJobs() const { return m_peakActive; }

signals:
    // On the scheduler's thread, once per capture that asked for it.
    void thumbnailReady(int captureId, const QString& filePath, const QString& thumbnailPath);
    void thumbnailFailed(int captureId, const QString& filePath);

private:
    struct Source
    {
        QString path;
        QString type;
        qint64 size = -1;
        QDateTime modified;
        QString key() const;
        static Source stat(const QString& path, const QString& type);
        bool exists() const { return size >= 0; }
    };
    struct Job
    {
        Source source;
        QVector<int> captureIds;
        int redecodes = 0;
    };

    void startNext();
    void finished(const QString& key, const QString& thumbnailPath);
    void rememberFailure(const QString& key);

    QString m_thumbnailsDir;
    Decoder m_decoder;
    QThreadPool m_pool;
    QHash<QString, Job> m_jobs;        // waiting or running, by source key
    QVector<QString> m_queue;          // waiting keys, oldest first
    QSet<QString> m_failed;            // unchanged sources that failed once
    int m_active = 0;
    int m_peakActive = 0;
    std::shared_ptr<std::atomic_bool> m_closing = std::make_shared<std::atomic_bool>(false);
};
