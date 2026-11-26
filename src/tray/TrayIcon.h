#pragma once
#include <QIcon>
#include <QObject>
#include <QSystemTrayIcon>

#include "tray/TrayIconRenderer.h"
#include "tray/TrayStatus.h"

class QMenu;
class QAction;
class ConfigManager;

// Tray icon + menu (docs/product-spec.md §16).
//
// The icon itself is a view of TrayStatus: the model decides what the user
// should see, TrayIconRenderer decides how to draw it, and this class only
// keeps QSystemTrayIcon in sync. Icon appearance follows
// ui.tray_status_display (badge | replace | static) and is live-applied.
class TrayIcon : public QObject
{
    Q_OBJECT
public:
    explicit TrayIcon(QObject* parent = nullptr, bool show = true);
    ~TrayIcon() override;

    // The status model behind the icon. Never null: it exists from
    // construction so wiring can happen before the tray is shown.
    TrayStatus* status() const { return m_status; }

    // Reads ui.tray_status_display and follows later changes to it, so
    // switching the mode in Settings repaints the tray without a restart.
    void setConfigManager(ConfigManager* config);
    TrayIconRenderer::Mode statusMode() const { return m_mode; }

    // Balloon notification (docs/product-spec.md §16); no-op if unsupported.
    void showNotification(const QString& title, const QString& body);
    QString openGalleryText() const;
    QAction* openGalleryAction() const { return m_openAction; }
    QAction* actionForId(const QString& id) const;
    int menuActionCount() const;

    QString tooltipText() const;
    QIcon trayIcon() const;

public slots:
    void retranslate();

signals:
    void openGalleryRequested();
    void rescanRequested();
    void screenshotRequested();
    void quitRequested();

private:
    void applyStatusMode(const QString& value);
    // Repaints icon + tooltip from the model. Cheap after the first call:
    // the renderer serves everything from its cache.
    void refreshTray();

    QSystemTrayIcon* m_tray;
    QMenu* m_menu;
    QAction* m_openAction = nullptr;
    QAction* m_rescanAction = nullptr;
    QAction* m_screenshotAction = nullptr;
    QAction* m_replayAction = nullptr;
    QAction* m_quitAction = nullptr;

    TrayStatus* m_status = nullptr;
    TrayIconRenderer m_renderer;
    TrayIconRenderer::Mode m_mode = TrayIconRenderer::Mode::Badge;
};
