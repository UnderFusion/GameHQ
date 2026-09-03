#include "input/ActionCatalog.h"
#include "localization/LanguageManager.h"
#include "localization/LocaleRegistry.h"
#include "tray/TrayIcon.h"

#include <QAction>
#include <QList>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>
#include <QVector>
#include <memory>

namespace
{
struct ExpectedSurfaceText
{
    QString qml;
    QString overlay;
    QString input;
    QString gallery;
    QString dialog;
    QString tray;
    QString model;
    QString releaseNotes;
};

ExpectedSurfaceText expectedFor(const QString &language)
{
    if (language == QLatin1String("pl-PL")) {
        return {QStringLiteral("Zobacz pełne informacje o wydaniu"),
                QStringLiteral("Gra nadal ma fokus i może reagować na sterowanie kontrolerem"),
                QStringLiteral("Urządzenia wejściowe"), QStringLiteral("Wybór wielu"),
                QStringLiteral("Usunąć zrzut?"),
                QStringLiteral("Otwórz galerię"), QStringLiteral("Zrzut ekranu"),
                QStringLiteral("Informacje o wydaniu")};
    }
    if (language == QLatin1String("zh-Hans")) {
        return {QStringLiteral("查看完整发行说明"),
                QStringLiteral("游戏仍处于焦点状态，可能会响应控制器输入"),
                QStringLiteral("输入设备"), QStringLiteral("批量选择"),
                QStringLiteral("删除捕获内容？"), QStringLiteral("打开图库"),
                QStringLiteral("屏幕截图"), QStringLiteral("发行说明")};
    }
    return {QStringLiteral("View full release notes"),
            QStringLiteral("The game still has focus and may react to controller input"),
            QStringLiteral("Input devices"), QStringLiteral("Bulk select"),
            QStringLiteral("Delete capture?"), QStringLiteral("Open Gallery"),
            QStringLiteral("Screenshot"), QStringLiteral("Release notes")};
}

QStringList expectedNativePresentation(const QString& language)
{
    if (language == QLatin1String("pl-PL")) {
        return {QStringLiteral("Skanuj zrzuty ponownie"),
                QStringLiteral("Zrób zrzut ekranu"),
                QStringLiteral("Zapisz powtórkę"), QStringLiteral("Wyjdź"),
                QStringLiteral("Zrób zrzut ekranu bieżącej gry."),
                QStringLiteral("Przejdź w górę"),
                QStringLiteral("Przenieś zaznaczenie w górę w nakładce.")};
    }
    if (language == QLatin1String("zh-Hans")) {
        return {QStringLiteral("重新扫描捕获内容"), QStringLiteral("截取屏幕截图"),
                QStringLiteral("保存回放"), QStringLiteral("退出"),
                QStringLiteral("捕获当前游戏的屏幕截图。"), QStringLiteral("向上导航"),
                QStringLiteral("在覆盖层中向上移动选择。")};
    }
    return {QStringLiteral("Rescan Captures"), QStringLiteral("Take Screenshot"),
            QStringLiteral("Save Replay"), QStringLiteral("Exit"),
            QStringLiteral("Capture a screenshot of the current game."),
            QStringLiteral("Navigate Up"),
            QStringLiteral("Move selection up in the overlay.")};
}
}

class LiveRetranslationTest : public QObject
{
    Q_OBJECT

private slots:
    void repeatedSwitchesRefreshEveryRepresentativeSurfaceAtomically();
    void failedSwitchRetainsTheCompletePreviousState();
    void switchingObjectsAndTranslatorStackAreReleased();
};

void LiveRetranslationTest::repeatedSwitchesRefreshEveryRepresentativeSurfaceAtomically()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.load(QStringLiteral(":/i18n/locales-test.json"), &error),
             qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY2(manager.initialize(QStringLiteral("en-US"), {}, &error), qPrintable(error));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("languageManager"), &manager);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQml
        QtObject {
            property string label: qsTrId("gamehq.about.full_release_notes")
            property string overlay: qsTrId("gamehq.overlay.focus_warning")
            property string input: qsTrId("gamehq.settings.input.devices.title")
            property string gallery: qsTrId("gamehq.gallery.action.bulk_select")
            property string dialog: qsTrId("gamehq.gallery.delete_capture.title")
            property string support: qsTrId("gamehq.navigation.support_gamehq")
            property string decimal: {
                languageManager.translationRevision
                return languageManager.formatDecimal(1234.5, 1)
            }
            property string fileSize: qsTrId("gamehq.format.size.megabytes")
                                      .arg(languageManager.formatDecimal(1.5, 1))
            property string oneMinute: qsTrId("gamehq.duration.minutes", 1)
            property string fiveMinutes: qsTrId("gamehq.duration.minutes", 5)
        }
    )", QUrl());
    std::unique_ptr<QObject> qmlObject(component.create());
    QVERIFY2(qmlObject, qPrintable(component.errorString()));

    TrayIcon tray(nullptr, false);
    ActionCatalog::retranslate();
    const int trayActionCount = tray.menuActionCount();
    const QStringList trayIds{QStringLiteral("open_gallery"), QStringLiteral("rescan"),
                              QStringLiteral("screenshot"), QStringLiteral("save_replay"),
                              QStringLiteral("quit")};
    QList<QAction*> trayActions;
    for (const QString& id : trayIds) {
        QAction* action = tray.actionForId(id);
        QVERIFY2(action, qPrintable(id));
        trayActions.append(action);
    }
    QVector<const ActionCatalog::Action*> actionObjects;
    QStringList actionIds;
    for (const auto& action : ActionCatalog::all()) {
        actionObjects.append(&action);
        actionIds.append(action.id);
    }
    QSignalSpy openSpy(&tray, &TrayIcon::openGalleryRequested);
    QSignalSpy rescanSpy(&tray, &TrayIcon::rescanRequested);
    QSignalSpy screenshotSpy(&tray, &TrayIcon::screenshotRequested);
    QSignalSpy quitSpy(&tray, &TrayIcon::quitRequested);
    QStringList sequence;
    connect(&manager, &LanguageManager::requestedLanguageChanged,
            this, [&sequence] { sequence.append(QStringLiteral("requested")); });
    connect(&manager, &LanguageManager::languageChanged,
            this, [&sequence] { sequence.append(QStringLiteral("language")); });
    connect(&manager, &LanguageManager::translationRevisionChanged,
            this, [&sequence] { sequence.append(QStringLiteral("revision")); });
    connect(&manager, &LanguageManager::retranslationRequested, this, [&] {
        ActionCatalog::retranslate();
        tray.retranslate();
        sequence.append(QStringLiteral("native"));
    });
    manager.setQmlRetranslateCallback([&] {
        engine.retranslate();
        sequence.append(QStringLiteral("qml"));
    });

    const auto verifySurfaces = [&](const QString &language) {
        const ExpectedSurfaceText expected = expectedFor(language);
        const QStringList native = expectedNativePresentation(language);
        QCOMPARE(manager.effectiveLanguage(), language);
        QCOMPARE(qmlObject->property("label").toString(), expected.qml);
        QCOMPARE(qmlObject->property("overlay").toString(), expected.overlay);
        QCOMPARE(qmlObject->property("input").toString(), expected.input);
        QCOMPARE(qmlObject->property("gallery").toString(), expected.gallery);
        QCOMPARE(qmlObject->property("dialog").toString(), expected.dialog);
        const QString expectedSupport = language == QLatin1String("pl-PL")
                                            ? QStringLiteral("Wesprzyj GameHQ")
                                        : language == QLatin1String("zh-Hans")
                                            ? QStringLiteral("支持 GameHQ")
                                            : QStringLiteral("Support GameHQ");
        QCOMPARE(qmlObject->property("support").toString(), expectedSupport);
        const QLocale locale(language);
        QCOMPARE(qmlObject->property("decimal").toString(),
                 locale.toString(1234.5, 'f', 1));
        QCOMPARE(qmlObject->property("fileSize").toString(),
                 QStringLiteral("%1 MB").arg(locale.toString(1.5, 'f', 1)));
        QVERIFY(!qmlObject->property("oneMinute").toString().contains(QStringLiteral("(s)")));
        QVERIFY(!qmlObject->property("fiveMinutes").toString().contains(QStringLiteral("(s)")));
        QCOMPARE(tray.openGalleryText(), expected.tray);
        QCOMPARE(tray.actionForId(QStringLiteral("rescan"))->text(), native.at(0));
        QCOMPARE(tray.actionForId(QStringLiteral("screenshot"))->text(), native.at(1));
        QCOMPARE(tray.actionForId(QStringLiteral("save_replay"))->text(), native.at(2));
        QCOMPARE(tray.actionForId(QStringLiteral("quit"))->text(), native.at(3));
        QCOMPARE(tray.menuActionCount(), trayActionCount);
        for (qsizetype index = 0; index < trayIds.size(); ++index)
            QCOMPARE(tray.actionForId(trayIds.at(index)), trayActions.at(index));
        QCOMPARE(ActionCatalog::all().size(), actionObjects.size());
        for (qsizetype index = 0; index < actionObjects.size(); ++index) {
            QCOMPARE(&ActionCatalog::all().at(index), actionObjects.at(index));
            QCOMPARE(ActionCatalog::all().at(index).id, actionIds.at(index));
            QVERIFY(!ActionCatalog::all().at(index).label.isEmpty());
            QVERIFY(!ActionCatalog::all().at(index).description.isEmpty());
        }
        const auto *action = ActionCatalog::find(QStringLiteral("global.screenshot"));
        QVERIFY(action);
        QCOMPARE(action->label, expected.model);
        QCOMPARE(action->description, native.at(4));
        const auto* overlayAction = ActionCatalog::find(QStringLiteral("overlay.navigate_up"));
        QVERIFY(overlayAction);
        QCOMPARE(overlayAction->label, native.at(5));
        QCOMPARE(overlayAction->description, native.at(6));
        QCOMPARE(qtTrId("gamehq.release_notes.title"), expected.releaseNotes);
    };
    verifySurfaces(QStringLiteral("en-US"));

    for (const QString &language : {QStringLiteral("pl-PL"),
                                    QStringLiteral("zh-Hans"),
                                    QStringLiteral("en-US")}) {
        sequence.clear();
        manager.setRequestedLanguage(language);
        QCOMPARE(sequence, QStringList({QStringLiteral("requested"),
                                        QStringLiteral("language"),
                                        QStringLiteral("revision"),
                                        QStringLiteral("native"),
                                        QStringLiteral("qml")}));
        verifySurfaces(language);
    }

    for (int cycle = 0; cycle < 8; ++cycle) {
        for (const QString &language : {QStringLiteral("pl-PL"),
                                        QStringLiteral("zh-Hans"),
                                        QStringLiteral("en-US")}) {
            manager.setRequestedLanguage(language);
            verifySurfaces(language);
        }
    }

    tray.actionForId(QStringLiteral("open_gallery"))->trigger();
    tray.actionForId(QStringLiteral("rescan"))->trigger();
    tray.actionForId(QStringLiteral("screenshot"))->trigger();
    tray.actionForId(QStringLiteral("quit"))->trigger();
    QCOMPARE(openSpy.size(), 1);
    QCOMPARE(rescanSpy.size(), 1);
    QCOMPARE(screenshotSpy.size(), 1);
    QCOMPARE(quitSpy.size(), 1);
}

void LiveRetranslationTest::failedSwitchRetainsTheCompletePreviousState()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.load(QStringLiteral(":/i18n/locales-test.json"), &error),
             qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY2(manager.initialize(QStringLiteral("pl-PL"), {}, &error), qPrintable(error));

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQml
        QtObject { property string label: qsTrId("gamehq.about.full_release_notes") }
    )", QUrl());
    std::unique_ptr<QObject> qmlObject(component.create());
    QVERIFY2(qmlObject, qPrintable(component.errorString()));
    TrayIcon tray(nullptr, false);
    ActionCatalog::retranslate();
    connect(&manager, &LanguageManager::retranslationRequested, this, [&] {
        ActionCatalog::retranslate();
        tray.retranslate();
    });
    int qmlRetranslates = 0;
    manager.setQmlRetranslateCallback([&] {
        ++qmlRetranslates;
        engine.retranslate();
    });
    QSignalSpy requestedSpy(&manager, &LanguageManager::requestedLanguageChanged);
    QSignalSpy languageSpy(&manager, &LanguageManager::languageChanged);
    QSignalSpy revisionSpy(&manager, &LanguageManager::translationRevisionChanged);
    QSignalSpy nativeSpy(&manager, &LanguageManager::retranslationRequested);
    QSignalSpy failureSpy(&manager, &LanguageManager::catalogLoadFailed);
    const int revision = manager.translationRevision();
    const QLocale previousLocale;

    manager.setRequestedLanguage(QStringLiteral("de-DE"));

    QCOMPARE(failureSpy.size(), 1);
    QCOMPARE(requestedSpy.size(), 0);
    QCOMPARE(languageSpy.size(), 0);
    QCOMPARE(revisionSpy.size(), 0);
    QCOMPARE(nativeSpy.size(), 0);
    QCOMPARE(qmlRetranslates, 0);
    QCOMPARE(manager.requestedLanguage(), QStringLiteral("pl-PL"));
    QCOMPARE(manager.effectiveLanguage(), QStringLiteral("pl-PL"));
    QCOMPARE(manager.translationRevision(), revision);
    QCOMPARE(QLocale().name(), previousLocale.name());
    const ExpectedSurfaceText expected = expectedFor(QStringLiteral("pl-PL"));
    QCOMPARE(qmlObject->property("label").toString(), expected.qml);
    QCOMPARE(tray.openGalleryText(), expected.tray);
    QCOMPARE(ActionCatalog::find(QStringLiteral("global.screenshot"))->label, expected.model);
    QCOMPARE(qtTrId("gamehq.release_notes.title"), expected.releaseNotes);
}

void LiveRetranslationTest::switchingObjectsAndTranslatorStackAreReleased()
{
    QList<QPointer<QAction>> trayActions;
    QPointer<QObject> qmlObject;
    {
        LocaleRegistry registry(false);
        QString error;
        QVERIFY2(registry.load(QStringLiteral(":/i18n/locales-test.json"), &error),
                 qPrintable(error));
        LanguageManager manager(&registry);
        QVERIFY2(manager.initialize(QStringLiteral("en-US"), {}, &error), qPrintable(error));
        manager.setRequestedLanguage(QStringLiteral("pl-PL"));
        manager.setRequestedLanguage(QStringLiteral("zh-Hans"));

        auto tray = std::make_unique<TrayIcon>(nullptr, false);
        for (const QString& id : {QStringLiteral("open_gallery"), QStringLiteral("rescan"),
                                  QStringLiteral("screenshot"), QStringLiteral("save_replay"),
                                  QStringLiteral("quit")}) {
            trayActions.append(tray->actionForId(id));
            QVERIFY(trayActions.constLast());
        }

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQml; QtObject {}", QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qmlObject = object.get();
    }

    for (const QPointer<QAction>& action : trayActions)
        QVERIFY(action.isNull());
    QVERIFY(qmlObject.isNull());
    QCOMPARE(qtTrId("gamehq.release_notes.title"),
             QStringLiteral("gamehq.release_notes.title"));
}

QTEST_MAIN(LiveRetranslationTest)
#include "tst_liveretranslation.moc"
