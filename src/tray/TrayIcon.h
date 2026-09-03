#pragma once
#include <QObject>
#include <QSystemTrayIcon>

class QMenu;
class QAction;

// Tray icon + menu (docs/product-spec.md §16). Capture actions appear as
// disabled placeholders until milestones 0.4/0.5 wire them up.
class TrayIcon : public QObject
{
    Q_OBJECT
public:
    explicit TrayIcon(QObject* parent = nullptr, bool show = true);
    ~TrayIcon() override;

    // Balloon notification (docs/product-spec.md §16); no-op if unsupported.
    void showNotification(const QString& title, const QString& body);
    QString openGalleryText() const;
    QAction* openGalleryAction() const { return m_openAction; }
    QAction* actionForId(const QString& id) const;
    int menuActionCount() const;

public slots:
    void retranslate();

signals:
    void openGalleryRequested();
    void rescanRequested();
    void screenshotRequested();
    void quitRequested();

private:
    QSystemTrayIcon* m_tray;
    QMenu* m_menu;
    QAction* m_openAction = nullptr;
    QAction* m_rescanAction = nullptr;
    QAction* m_screenshotAction = nullptr;
    QAction* m_replayAction = nullptr;
    QAction* m_quitAction = nullptr;
};
