#include "input/ActionCatalog.h"
#include "localization/LanguageManager.h"
#include "localization/LocaleRegistry.h"
#include "tray/TrayIcon.h"

#include <QAction>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>
#include <memory>

namespace
{
struct ExpectedSurfaceText
{
    QString qml;
    QString tray;
    QString model;
    QString releaseNotes;
};

ExpectedSurfaceText expectedFor(const QString &language)
{
    if (language == QLatin1String("pl-PL")) {
        return {QStringLiteral("Zobacz pełne informacje o wydaniu"),
                QStringLiteral("Otwórz galerię"), QStringLiteral("Zrzut ekranu"),
                QStringLiteral("Informacje o wydaniu")};
    }
    if (language == QLatin1String("zh-Hans")) {
        return {QStringLiteral("查看完整发行说明"), QStringLiteral("打开图库"),
                QStringLiteral("屏幕截图"), QStringLiteral("发行说明")};
    }
    return {QStringLiteral("See full release notes"), QStringLiteral("Open Gallery"),
            QStringLiteral("Screenshot"), QStringLiteral("Release notes")};
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
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQml
        QtObject { property string label: qsTrId("gamehq.about.full_release_notes") }
    )", QUrl());
    std::unique_ptr<QObject> qmlObject(component.create());
    QVERIFY2(qmlObject, qPrintable(component.errorString()));

    TrayIcon tray(nullptr, false);
    ActionCatalog::retranslate();
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
        QCOMPARE(manager.effectiveLanguage(), language);
        QCOMPARE(qmlObject->property("label").toString(), expected.qml);
        QCOMPARE(tray.openGalleryText(), expected.tray);
        const auto *action = ActionCatalog::find(QStringLiteral("global.screenshot"));
        QVERIFY(action);
        QCOMPARE(action->label, expected.model);
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
    QPointer<QAction> trayAction;
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
        trayAction = tray->openGalleryAction();
        QVERIFY(trayAction);

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQml; QtObject {}", QUrl());
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        qmlObject = object.get();
    }

    QVERIFY(trayAction.isNull());
    QVERIFY(qmlObject.isNull());
    QCOMPARE(qtTrId("gamehq.release_notes.title"),
             QStringLiteral("gamehq.release_notes.title"));
}

QTEST_MAIN(LiveRetranslationTest)
#include "tst_liveretranslation.moc"
