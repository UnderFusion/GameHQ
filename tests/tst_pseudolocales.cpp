#include "localization/LanguageManager.h"
#include "localization/LocaleRegistry.h"

#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTest>
#include <memory>

namespace {

QStringList availableTags(const LocaleRegistry& registry)
{
    QStringList tags;
    for (const QVariant& value : registry.availableLanguages())
        tags.append(value.toMap().value(QStringLiteral("tag")).toString());
    return tags;
}

void collectNamedItems(QQuickItem* parent, const QString& name, QList<QQuickItem*>& result)
{
    for (QQuickItem* child : parent->childItems()) {
        if (child->objectName() == name)
            result.append(child);
        collectNamedItems(child, name, result);
    }
}

} // namespace

class PseudoLocalesTest : public QObject
{
    Q_OBJECT

private slots:
    void productionAndDevelopmentBoundariesAreDistinct();
    void generatedCatalogsPreserveRuntimeContracts();
    void expandedAndRtlDenseSurfacesDoNotOverflow();
    void directionalIconPolicyIsExplicit();
};

void PseudoLocalesTest::productionAndDevelopmentBoundariesAreDistinct()
{
    QFile productionFile(QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/locales.json"));
    QVERIFY(productionFile.open(QIODevice::ReadOnly));
    LocaleRegistry production(false);
    QString error;
    QVERIFY2(production.loadData(productionFile.readAll(), &error), qPrintable(error));
    QCOMPARE(availableTags(production).size(), 12);
    QVERIFY(production.catalogName(QStringLiteral("en-XA")).isEmpty());
    QVERIFY(production.catalogName(QStringLiteral("ar-XB")).isEmpty());
    QCOMPARE(production.resolveAvailable(QStringLiteral("en-XA")), QStringLiteral("en-US"));
    QCOMPARE(production.resolveAvailable(QStringLiteral("ar-XB")), QStringLiteral("en-US"));

    LocaleRegistry development(true);
    QVERIFY2(development.load(QStringLiteral(":/pseudo-i18n/locales.json"), &error),
             qPrintable(error));
    const QStringList tags = availableTags(development);
    QCOMPARE(tags.size(), 14);
    QVERIFY(tags.contains(QStringLiteral("en-XA")));
    QVERIFY(tags.contains(QStringLiteral("ar-XB")));
    QCOMPARE(development.layoutDirection(QStringLiteral("en-XA")), Qt::LeftToRight);
    QCOMPARE(development.layoutDirection(QStringLiteral("ar-XB")), Qt::RightToLeft);
}

void PseudoLocalesTest::generatedCatalogsPreserveRuntimeContracts()
{
    LocaleRegistry registry(true);
    QString error;
    QVERIFY2(registry.load(QStringLiteral(":/pseudo-i18n/locales.json"), &error),
             qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY2(manager.initialize(QStringLiteral("en-XA"), {}, &error), qPrintable(error));

    const QString expanded = qtTrId("gamehq.about.product_description");
    QVERIFY(expanded.startsWith(QStringLiteral("⟦")));
    QVERIFY(expanded.endsWith(QStringLiteral("⟧")));
    const QString source = QStringLiteral(
        "A controller-friendly screenshot, replay, and media gallery for PC games.");
    QVERIFY(expanded.size() > source.size());
    const QString parameterized = qtTrId("gamehq.player.position_duration");
    QVERIFY(parameterized.contains(QStringLiteral("%1")));
    QVERIFY(parameterized.contains(QStringLiteral("%2")));
    QVERIFY(qtTrId("gamehq.about.enjoying_gamehq").contains(QStringLiteral("GameHQ")));
    QVERIFY(!expanded.contains(QStringLiteral("gamehq.")));

    manager.setRequestedLanguage(QStringLiteral("ar-XB"));
    QCOMPARE(manager.effectiveLanguage(), QStringLiteral("ar-XB"));
    QCOMPARE(manager.layoutDirection(), Qt::RightToLeft);
    const QString rtl = qtTrId("gamehq.about.product_description");
    QVERIFY(rtl.startsWith(QStringLiteral("⟫\u2067")));
    QVERIFY(rtl.endsWith(QStringLiteral("\u2069⟪")));
    QVERIFY(qtTrId("gamehq.about.enjoying_gamehq").contains(QStringLiteral("GameHQ")));
    QVERIFY(qtTrId("gamehq.player.position_duration").contains(QStringLiteral("%1")));
}

void PseudoLocalesTest::expandedAndRtlDenseSurfacesDoNotOverflow()
{
    LocaleRegistry registry(true);
    QString error;
    QVERIFY2(registry.load(QStringLiteral(":/pseudo-i18n/locales.json"), &error),
             qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY2(manager.initialize(QStringLiteral("en-XA"), {}, &error), qPrintable(error));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("languageManager"), &manager);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Item {
            id: root
            width: 520
            height: denseColumn.implicitHeight
            LayoutMirroring.enabled: languageManager.layoutDirection === Qt.RightToLeft
            LayoutMirroring.childrenInherit: true
            property bool rtlMirrored: LayoutMirroring.enabled
            property var messageIds: [
                "gamehq.settings.language.description",
                "gamehq.navigation.settings",
                "gamehq.settings.general.description",
                "gamehq.input.binding_card.add_input",
                "gamehq.gallery.delete_capture.message",
                "gamehq.overlay.focus_warning",
                "gamehq.update.new_version_available",
                "gamehq.release_notes.title",
                "gamehq.tray.open_gallery",
                "gamehq.notification.settings_quarantined.body",
                "gamehq.error.capture_location.not_writable",
                "gamehq.about.product_description"
            ]
            Column {
                id: denseColumn
                width: parent.width
                spacing: 4
                Repeater {
                    model: root.messageIds
                    Text {
                        objectName: "densePseudoText"
                        width: denseColumn.width
                        height: implicitHeight
                        text: {
                            languageManager.translationRevision
                            return qsTrId(modelData)
                        }
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    )", QUrl(QStringLiteral("qrc:/tests/PseudoDense.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickItem* rootItem = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(rootItem);
    manager.setQmlRetranslateCallback([&engine] { engine.retranslate(); });

    const auto verify = [&](const QString& delimiter, bool rtl) {
        QCoreApplication::processEvents();
        QList<QQuickItem*> labels;
        collectNamedItems(rootItem, QStringLiteral("densePseudoText"), labels);
        QCOMPARE(labels.size(), 12);
        QCOMPARE(root->property("rtlMirrored").toBool(), rtl);
        for (QQuickItem* label : labels) {
            const QString text = label->property("text").toString();
            QVERIFY2(text.startsWith(delimiter), qPrintable(text));
            QVERIFY(!text.contains(QStringLiteral("gamehq.")));
            QVERIFY(!label->property("truncated").toBool());
            QVERIFY(label->property("paintedWidth").toReal()
                    <= label->property("width").toReal() + 1.0);
            QVERIFY(label->property("height").toReal()
                    >= label->property("paintedHeight").toReal());
        }
    };

    verify(QStringLiteral("⟦"), false);
    manager.setRequestedLanguage(QStringLiteral("ar-XB"));
    verify(QStringLiteral("⟫\u2067"), true);
    manager.setRequestedLanguage(QStringLiteral("en-XA"));
    verify(QStringLiteral("⟦"), false);
}

void PseudoLocalesTest::directionalIconPolicyIsExplicit()
{
    const auto read = [](const QString& relative) {
        QFile file(QStringLiteral(GAMEHQ_SOURCE_DIR "/src/ui/qml/") + relative);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    for (const QString& root : {QStringLiteral("Main.qml"), QStringLiteral("OverlayWindow.qml"),
                                QStringLiteral("ToastWindow.qml")}) {
        const QString source = read(root);
        QVERIFY2(source.contains(QStringLiteral("LayoutMirroring.enabled")), qPrintable(root));
        QVERIFY2(source.contains(QStringLiteral("LayoutMirroring.childrenInherit: true")),
                 qPrintable(root));
    }
    QVERIFY(read(QStringLiteral("components/AboutWhatsNewDialog.qml"))
                .contains(QStringLiteral("layoutDirection === Qt.RightToLeft ? \"›\" : \"‹\"")));
    QVERIFY(read(QStringLiteral("components/TextLink.qml"))
                .contains(QStringLiteral("function mirroredSuffix")));
    QVERIFY(read(QStringLiteral("components/SettingsLinkRow.qml"))
                .contains(QStringLiteral("Qt.RightToLeft ? \"\\u2039\" : \"\\u203A\"")));
    QVERIFY(read(QStringLiteral("components/BindingCard.qml"))
                .contains(QStringLiteral("function directionalActionLabel")));
    const QString player = read(QStringLiteral("components/PlayerControls.qml"));
    QVERIFY(player.contains(QStringLiteral("text: \"<<\"")));
    QVERIFY(player.contains(QStringLiteral("text: \">>\"")));
}

QTEST_MAIN(PseudoLocalesTest)
#include "tst_pseudolocales.moc"
