#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;
class QWinEventNotifier;

// Tells the library when files appear, disappear or get renamed under a
// capture root, so the gallery follows Explorer deletes and Recycle Bin
// restores without a manual rescan.
//
// One Windows change notification per root, watching the whole subtree for
// name changes only. Unlike QFileSystemWatcher (a handle per directory) this
// never holds subfolders open, so the user can still delete a game folder.
// Size/timestamp writes are not watched: a clip being encoded does not wake
// the scanner. Events are debounced and reported per root.
class CaptureFolderWatcher : public QObject
{
    Q_OBJECT
public:
    explicit CaptureFolderWatcher(QObject* parent = nullptr);
    ~CaptureFolderWatcher() override;

    // Re-arms the watches; unchanged roots keep their handle. Roots that do
    // not exist (offline drive) are skipped until the next call.
    void setRoots(const QStringList& roots);
    QStringList roots() const;

    // Reports these roots again after delayMs even without a new event.
    void recheckLater(const QStringList& roots, int delayMs);

    static constexpr int kDebounceMs = 600;

signals:
    void rootsChanged(const QStringList& roots);

private:
    struct Watch
    {
        QString root;
        void* handle = nullptr;               // HANDLE from FindFirstChangeNotificationW
        QWinEventNotifier* notifier = nullptr;
    };

    void onSignalled(int watchIndex);
    void flush();
    void closeWatch(Watch& watch);

    QVector<Watch> m_watches;
    QSet<QString> m_pending;
    QSet<QString> m_recheck;
    QTimer* m_debounce = nullptr;
    QTimer* m_recheckTimer = nullptr;
};
