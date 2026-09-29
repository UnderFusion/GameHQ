#include "storage/CaptureFolderWatcher.h"

#include <QDebug>
#include <QDir>
#include <QTimer>
#include <QWinEventNotifier>

#include <windows.h>

CaptureFolderWatcher::CaptureFolderWatcher(QObject* parent)
    : QObject(parent)
    , m_debounce(new QTimer(this))
    , m_recheckTimer(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &CaptureFolderWatcher::flush);
    m_recheckTimer->setSingleShot(true);
    connect(m_recheckTimer, &QTimer::timeout, this, [this] {
        m_pending.unite(m_recheck);
        m_recheck.clear();
        flush();
    });
}

CaptureFolderWatcher::~CaptureFolderWatcher()
{
    for (Watch& watch : m_watches)
        closeWatch(watch);
}

void CaptureFolderWatcher::setRoots(const QStringList& roots)
{
    QStringList wanted;
    for (const QString& root : roots) {
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(root));
        if (!clean.isEmpty() && !wanted.contains(clean, Qt::CaseInsensitive) && QDir(clean).exists())
            wanted.append(clean);
    }

    QVector<Watch> kept;
    for (Watch& watch : m_watches) {
        if (watch.handle && wanted.contains(watch.root, Qt::CaseInsensitive)) {
            wanted.removeIf([&](const QString& root) {
                return root.compare(watch.root, Qt::CaseInsensitive) == 0;
            });
            kept.append(watch);
        } else {
            closeWatch(watch);
        }
    }
    m_watches = kept;

    for (const QString& root : wanted) {
        const std::wstring native = QDir::toNativeSeparators(root).toStdWString();
        HANDLE handle = FindFirstChangeNotificationW(
            native.c_str(), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        if (handle == INVALID_HANDLE_VALUE) {
            qWarning() << "Library watch: cannot watch" << root << "error" << GetLastError();
            continue;
        }
        m_watches.append({ root, handle, nullptr });
    }

    // Notifiers carry the index they report, so rebuild them after the list
    // settles instead of patching indices.
    for (int i = 0; i < m_watches.size(); ++i) {
        Watch& watch = m_watches[i];
        delete watch.notifier;
        watch.notifier = new QWinEventNotifier(static_cast<HANDLE>(watch.handle), this);
        connect(watch.notifier, &QWinEventNotifier::activated, this, [this, i] { onSignalled(i); });
    }
    qInfo() << "Library watch: watching" << m_watches.size() << "capture folder(s)";
}

QStringList CaptureFolderWatcher::roots() const
{
    QStringList out;
    for (const Watch& watch : m_watches)
        out.append(watch.root);
    return out;
}

void CaptureFolderWatcher::recheckLater(const QStringList& roots, int delayMs)
{
    for (const QString& root : roots)
        m_recheck.insert(root);
    if (!m_recheckTimer->isActive())
        m_recheckTimer->start(delayMs);
}

void CaptureFolderWatcher::onSignalled(int watchIndex)
{
    if (watchIndex < 0 || watchIndex >= m_watches.size())
        return;
    Watch& watch = m_watches[watchIndex];
    m_pending.insert(watch.root);
    // Re-arm first; if the root itself vanished, close the watch rather than
    // spin on a handle that keeps signalling. The next setRoots() reopens it.
    if (!watch.handle || !FindNextChangeNotification(static_cast<HANDLE>(watch.handle))) {
        qWarning() << "Library watch: lost" << watch.root << "error" << GetLastError();
        if (watch.notifier)
            watch.notifier->setEnabled(false);
        if (watch.handle) {
            FindCloseChangeNotification(static_cast<HANDLE>(watch.handle));
            watch.handle = nullptr;
        }
    }
    m_debounce->start();
}

void CaptureFolderWatcher::flush()
{
    if (m_pending.isEmpty())
        return;
    const QStringList roots(m_pending.cbegin(), m_pending.cend());
    m_pending.clear();
    emit rootsChanged(roots);
}

void CaptureFolderWatcher::closeWatch(Watch& watch)
{
    if (watch.notifier) {
        watch.notifier->setEnabled(false);
        delete watch.notifier;
        watch.notifier = nullptr;
    }
    if (watch.handle) {
        FindCloseChangeNotification(static_cast<HANDLE>(watch.handle));
        watch.handle = nullptr;
    }
}
