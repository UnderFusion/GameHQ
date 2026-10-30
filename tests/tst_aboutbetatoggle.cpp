// Real popup, SettingsToggle and shared settings rows; no running GameHQ instance.
#include "config/ConfigManager.h"
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <memory>

class PopupConfig : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool portableMode READ portableMode CONSTANT)
    Q_PROPERTY(QVariantList releaseNotesSections READ sections CONSTANT)
    Q_PROPERTY(QVariantList releaseNotesReleases READ sections CONSTANT)
    Q_PROPERTY(QString releaseNotesTitle READ title CONSTANT)
    Q_PROPERTY(int layoutDirection READ layoutDirection CONSTANT)
public:
    explicit PopupConfig(const QString &path) : m_config(path) {
        m_config.load();
        connect(&m_config, &ConfigManager::valueChanged, this, &PopupConfig::configChanged);
    }
    QString version() const { return QStringLiteral("0.8.1"); }
    bool portableMode() const { return true; }
    QVariantList sections() const { return {}; }
    QString title() const { return QStringLiteral("GameHQ hotfix"); }
    int layoutDirection() const { return Qt::LeftToRight; }
    Q_INVOKABLE QVariant config(const QString &key, const QVariant &fallback = {}) const {
        return m_config.value(key, fallback);
    }
    Q_INVOKABLE void setConfig(const QString &key, const QVariant &value) {
        m_config.setValue(key, value);
        m_config.save();
    }
    int navigationTicks = 0;
    Q_INVOKABLE void play(const QString &name) {
        if (name == QStringLiteral("nav_tick")) ++navigationTicks;
    }
    Q_INVOKABLE QString formatDecimal(double value, int precision) const {
        return QString::number(value, 'f', precision);
    }
    Q_INVOKABLE QString formatInteger(int value) const { return QString::number(value); }
    Q_INVOKABLE QString formatDateTime(const QDateTime &date) const { return date.toString(Qt::ISODate); }
    Q_INVOKABLE QString formatDate(const QDateTime &date) const { return date.toString(Qt::ISODate); }
signals:
    void configChanged(const QString &key, const QVariant &value);
    void configGroupReset(const QString &prefix);
private:
    ConfigManager m_config;
};

class PopupUpdates : public QQmlPropertyMap
{
    Q_OBJECT
public:
    PopupUpdates() : QQmlPropertyMap(this, nullptr) {}
    int installRequests = 0;
    int downloadRequests = 0;
    Q_INVOKABLE void downloadAndInstall() {
        ++installRequests;
        insert(QStringLiteral("stateName"), QStringLiteral("Downloading"));
    }
    Q_INVOKABLE void downloadUpdate() {
        ++downloadRequests;
        insert(QStringLiteral("stateName"), QStringLiteral("Downloading"));
    }
    Q_INVOKABLE void cancelDownload() {
        insert(QStringLiteral("stateName"), QStringLiteral("UpdateAvailable"));
    }
};

class AboutBetaToggleTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QTranslator m_english;
    std::unique_ptr<PopupConfig> m_app;
    std::unique_ptr<PopupUpdates> m_updates;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QQuickWindow> m_window;
    QObject *m_dialog = nullptr;
    const QString m_key = QStringLiteral("updates.include_betas");

    bool loadWindow() {
        m_app = std::make_unique<PopupConfig>(m_dir.filePath(QStringLiteral("config.json")));
        m_updates = std::make_unique<PopupUpdates>();
        for (const auto &name : {"latestVersion", "errorText", "releaseUrl"})
            m_updates->insert(QString::fromLatin1(name), QString());
        m_updates->insert(QStringLiteral("stateName"), QStringLiteral("UpToDate"));
        m_updates->insert(QStringLiteral("lastChecked"), QDateTime::currentDateTime());
        m_updates->insert(QStringLiteral("publishedAt"), QDateTime::currentDateTime());
        m_updates->insert(QStringLiteral("progress"), 0);
        m_updates->insert(QStringLiteral("size"), 0);
        m_updates->insert(QStringLiteral("failedDuringCheck"), false);
        m_updates->insert(QStringLiteral("waitingForStable"), false);
        m_updates->insert(QStringLiteral("notesLoading"), false);
        m_updates->insert(QStringLiteral("noteBlocks"), QVariantList());
        m_engine = std::make_unique<QQmlEngine>();
        m_engine->addImportPath(QStringLiteral(GAMEHQ_SOURCE_DIR "/tests/qml/imports"));
        for (const auto &name : {"app", "languageManager", "sounds"})
            m_engine->rootContext()->setContextProperty(QString::fromLatin1(name), m_app.get());
        m_engine->rootContext()->setContextProperty(QStringLiteral("updates"), m_updates.get());
        QQmlComponent component(m_engine.get());
        component.setData(R"qml(
import QtQuick
import "../src/ui/qml/components"
Window {
    width: 1000; height: 800; visible: true
    UpdateBanner { objectName: "banner"; anchors.top: parent.top; width: parent.width }
    AboutWhatsNewDialog { objectName: "dialog"; anchors.fill: parent; z: 10 }
    BetaUpdatesRow { objectName: "generalBeta"; visible: false }
    BetaUpdatesRow { objectName: "aboutBeta"; visible: false }
}
)qml", QUrl::fromLocalFile(QStringLiteral(GAMEHQ_SOURCE_DIR "/tests/about-beta-harness.qml")));
        m_window.reset(qobject_cast<QQuickWindow *>(component.create()));
        if (!m_window) {
            qWarning().noquote() << component.errorString();
            return false;
        }
        m_dialog = m_window->findChild<QObject *>(QStringLiteral("dialog"));
        return m_dialog && QMetaObject::invokeMethod(m_dialog, "open", Q_ARG(QVariant, false));
    }
    QQuickItem *popupToggle() const {
        for (auto *item : m_dialog->findChildren<QQuickItem *>(QStringLiteral("popupBetaToggle")))
            if (item->isVisible()) return item;
        return nullptr;
    }
    void checkSynchronized(bool value) {
        QCOMPARE(m_app->config(m_key, false).toBool(), value);
        for (const auto &row : {QStringLiteral("generalBeta"), QStringLiteral("aboutBeta")}) {
            QObject *toggle = nullptr;
            for (auto *item : m_window->findChild<QObject *>(row)->findChildren<QObject *>())
                if (item->property("configKey").toString() == m_key) toggle = item;
            QVERIFY(toggle);
            QCOMPARE(toggle->property("checked").toBool(), value);
        }
        for (auto *toggle : m_dialog->findChildren<QQuickItem *>(QStringLiteral("popupBetaToggle")))
            QCOMPARE(toggle->property("checked").toBool(), value);
    }
    void focusUsingController(QQuickItem *toggle) {
        for (int step = 0; step < 25 && !toggle->hasActiveFocus(); ++step)
            QVERIFY(QMetaObject::invokeMethod(m_dialog, "padStep", Q_ARG(QVariant, 1)));
        QVERIFY(toggle->hasActiveFocus());
        const QPointF position = toggle->mapToScene(QPointF(0, 0));
        QVERIFY(position.y() >= 0 && position.y() + toggle->height() <= m_window->height());
    }
    void savePreview(const QString &name) {
        const QString folder = qEnvironmentVariable("GAMEHQ_ABOUT_TOGGLE_SCREENSHOT_DIR");
        if (!folder.isEmpty()) {
            QTest::qWait(150);
            const QImage image = m_window->grabWindow();
            QVERIFY(!image.isNull());
            QVERIFY(image.save(folder + QLatin1Char('/') + name));
        }
    }
private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        const QString qm = qEnvironmentVariable("GAMEHQ_TEST_ENGLISH_QM");
        if (!qm.isEmpty()) {
            QVERIFY(m_english.load(qm));
            QCoreApplication::installTranslator(&m_english);
        }
    }
    void init() {
        QFile::remove(m_dir.filePath(QStringLiteral("config.json")));
        QVERIFY(loadWindow());
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QTRY_VERIFY(popupToggle());
        // Mouse input must wait for the popup's opening transition, even when
        // screenshot capture (which used to supply an incidental wait) is off.
        QTRY_COMPARE(m_dialog->property("opacity").toReal(), 1.0);
    }
    void cleanup() {
        m_window.reset(); m_engine.reset(); m_updates.reset(); m_app.reset();
        m_dialog = nullptr;
    }
    void compactMouseKeyboardAndControllerStaySynchronized() {
        auto *toggle = popupToggle();
        focusUsingController(toggle);
        savePreview(QStringLiteral("beta-toggle-compact.png"));
        const QPoint center = toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint();
        QTest::mouseClick(m_window.get(), Qt::LeftButton, Qt::NoModifier, center);
        checkSynchronized(true);
        QTest::keyClick(m_window.get(), Qt::Key_Space);
        checkSynchronized(false);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "padConfirm"));
        checkSynchronized(true);
        m_app->setConfig(m_key, false); // the same config path used by either settings page
        checkSynchronized(false);
    }
    void availableUpdateAndFullNotesKeepTheToggleReachable() {
        m_updates->insert(QStringLiteral("latestVersion"), QStringLiteral("0.8.2-beta1"));
        m_updates->insert(QStringLiteral("stateName"), QStringLiteral("UpdateAvailable"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "open", Q_ARG(QVariant, false)));
        QVERIFY(m_dialog->property("showFullNotes").toBool());
        QTRY_VERIFY(popupToggle());
        focusUsingController(popupToggle());
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "padConfirm"));
        checkSynchronized(true);
        savePreview(QStringLiteral("beta-toggle-update.png"));
        m_updates->insert(QStringLiteral("latestVersion"), QString());
        m_updates->insert(QStringLiteral("stateName"), QStringLiteral("UpToDate"));
        m_updates->insert(QStringLiteral("waitingForStable"), true);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openReleaseNotes"));
        QTRY_VERIFY(popupToggle());
        bool waitingVisible = false;
        for (auto *item : m_dialog->findChildren<QQuickItem *>(QStringLiteral("waitingStableStatus")))
            waitingVisible = waitingVisible || item->isVisible();
        QVERIFY(waitingVisible);
        focusUsingController(popupToggle());
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "padConfirm"));
        checkSynchronized(false);
    }
    void preferenceSurvivesRecreatingTheBackendAndPopup() {
        m_app->setConfig(m_key, true);
        m_window.reset(); m_engine.reset(); m_updates.reset(); m_app.reset();
        QVERIFY(loadWindow());
        QTRY_VERIFY(popupToggle());
        checkSynchronized(true);
    }
    void repeatedNavigationStopsTickingAtTheEdges() {
        for (int step = 0; step < 40; ++step)
            QVERIFY(QMetaObject::invokeMethod(m_dialog, "padStep", Q_ARG(QVariant, 1)));
        const int atEnd = m_app->navigationTicks;
        for (int step = 0; step < 8; ++step)
            QVERIFY(QMetaObject::invokeMethod(m_dialog, "padStep", Q_ARG(QVariant, 1)));
        QCOMPARE(m_app->navigationTicks, atEnd);
        for (int step = 0; step < 40; ++step)
            QVERIFY(QMetaObject::invokeMethod(m_dialog, "padStep", Q_ARG(QVariant, -1)));
        const int atStart = m_app->navigationTicks;
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "padStep", Q_ARG(QVariant, -1)));
        QCOMPARE(m_app->navigationTicks, atStart);
    }
    void downloadProgressStaysAboveScrollingNotesAndBannerInstallsDirectly() {
        m_updates->insert(QStringLiteral("latestVersion"), QStringLiteral("0.8.2-beta1"));
        m_updates->insert(QStringLiteral("stateName"), QStringLiteral("UpdateAvailable"));
        QVariantList notes;
        for (int i = 0; i < 40; ++i)
            notes.push_back(QVariantMap{{QStringLiteral("kind"), QStringLiteral("paragraph")},
                {QStringLiteral("text"), QStringLiteral("A longer release note to exercise scrolling while download progress stays visible.")},
                {QStringLiteral("lead"), QString()}, {QStringLiteral("body"), QString()}});
        m_updates->insert(QStringLiteral("noteBlocks"), notes);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "open", Q_ARG(QVariant, false)));
        auto *flick = m_dialog->findChild<QQuickItem *>(QStringLiteral("releaseNotesFlick"));
        auto *card = m_dialog->findChild<QQuickItem *>(QStringLiteral("updateActionCard"));
        auto *progress = m_dialog->findChild<QQuickItem *>(QStringLiteral("dialogUpdateProgress"));
        auto *status = m_dialog->findChild<QQuickItem *>(QStringLiteral("dialogUpdateStatus"));
        QVERIFY(flick && card && progress && status);
        QTRY_VERIFY(flick->property("contentHeight").toReal() > flick->height());
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "runPrimaryAction", Q_ARG(QVariant, true)));
        QCOMPARE(m_updates->downloadRequests, 1);
        m_updates->insert(QStringLiteral("progress"), 42);
        QTRY_VERIFY(progress->isVisible());
        QVERIFY(status->property("text").toString().contains(QStringLiteral("42")));
        const QPointF pinned = card->mapToScene(QPointF(0, 0));
        flick->setProperty("contentY", flick->property("contentHeight").toReal() - flick->height());
        QCOMPARE(card->mapToScene(QPointF(0, 0)), pinned);
        QVERIFY(progress->mapToScene(QPointF(0, 0)).y() + progress->height() <= m_window->height());
        savePreview(QStringLiteral("update-download-progress.png"));
        m_updates->cancelDownload();
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "close"));
        auto *install = m_window->findChild<QQuickItem *>(QStringLiteral("bannerInstallAction"));
        auto *details = m_window->findChild<QQuickItem *>(QStringLiteral("bannerDetailsAction"));
        QTRY_VERIFY(install && install->isVisible() && install->width() > 0);
        auto *banner = m_window->findChild<QQuickItem *>(QStringLiteral("banner"));
        QVERIFY(banner);
        // The height animation clips button hit targets until the banner has expanded.
        QTRY_COMPARE(banner->height(), banner->implicitHeight());
        QVERIFY(!m_window->grabWindow().isNull()); // finish the changed banner layout before clicking
        QVERIFY(install->property("primary").toBool());
        QVERIFY(install->x() < details->x());
        const QPoint center = install->mapToScene(QPointF(install->width() / 2, install->height() / 2)).toPoint();
        QTest::mouseClick(m_window.get(), Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(m_updates->installRequests, 1);
        QCOMPARE(m_updates->value(QStringLiteral("stateName")).toString(), QStringLiteral("Downloading"));
    }
};
QTEST_MAIN(AboutBetaToggleTest)
#include "tst_aboutbetatoggle.moc"
