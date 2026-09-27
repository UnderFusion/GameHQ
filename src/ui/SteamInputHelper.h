#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class ConfigManager;

// Steam Input conflict help (Settings → Input → Steam Input).
//
// Guidance only. When the game in session is a Steam install and a GameHQ
// controller shortcut uses Create/Share or PS, Steam Input MAY also send that
// button to the game. GameHQ cannot know whether it does: it never reads or
// writes Steam's controller configuration, never hooks Steam and never changes
// a layout. It only explains the possibility, and opens Steam's own screens
// when — and only when — the user presses a button for it.
//
// Exposed to QML as "steamInput".
class SteamInputHelper : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool steamGame READ steamGame NOTIFY changed)
    Q_PROPERTY(QString gameName READ gameName NOTIFY changed)
    // Bound advised controls ("gamepad.capture", "gamepad.guide").
    Q_PROPERTY(QStringList boundControls READ boundControls NOTIFY changed)
    // The subset the user has not reviewed for this game yet.
    Q_PROPERTY(QStringList pendingControls READ pendingControls NOTIFY changed)
    Q_PROPERTY(bool createShareBound READ createShareBound NOTIFY changed)
    Q_PROPERTY(bool guideBound READ guideBound NOTIFY changed)
public:
    struct Providers {
        std::function<QString()> gameExecutable;   // session game's executable path/key
        std::function<QString()> gameName;         // session game's display name
        std::function<QStringList()> boundControls; // advised controls bound to Global actions
        std::function<bool(const QString& url)> openUrl; // user-triggered only
        // One passive notice per game and run. Receives title/body.
        std::function<void(const QString&, const QString&)> notify;
    };

    SteamInputHelper(ConfigManager* config, Providers providers, QObject* parent = nullptr);

    bool enabled() const;
    bool steamGame() const { return !m_appId.isEmpty(); }
    QString appId() const { return m_appId; }
    QString gameName() const { return m_gameName; }
    QStringList boundControls() const { return m_bound; }
    QStringList pendingControls() const { return m_pending; }
    bool createShareBound() const;
    bool guideBound() const;

    // Every external action below runs only from an explicit user click.
    Q_INVOKABLE bool openGameLayout();
    Q_INVOKABLE bool openControllerSettings();
    Q_INVOKABLE void markReviewed();      // re-shows if the GameHQ mapping later changes
    Q_INVOKABLE void hideForGame();       // never shows again for this game

    // Translated button name for the advised controls.
    Q_INVOKABLE static QString controlLabel(const QString& control);
    static QString ackKey(const QString& appId);

public slots:
    // Re-evaluate the session game and the bound controls. Cheap: the Steam
    // manifest lookup runs only when the session game changes.
    void refresh();

signals:
    void changed();

private:
    void maybeNotify();

    ConfigManager* m_config;
    Providers m_providers;
    QString m_gameKey;       // session executable the cached app id belongs to
    QString m_appId;
    QString m_gameName;
    QStringList m_bound;
    QStringList m_pending;
    QSet<QString> m_notifiedThisRun;   // "<appid>|<pending>" already announced
};
