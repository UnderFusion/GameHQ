#include <QtTest>
#include <QFile>
#include <QJSEngine>
#include <QRegularExpression>

// Execute the shipped QML functions, rather than a second copy of their logic.
// UI objects are represented only by the collaborators those functions call.
class RecentUiTest : public QObject
{
    Q_OBJECT
    static QString source(const char* path)
    {
        QFile file(QStringLiteral(GAMEHQ_SOURCE_DIR) + QLatin1Char('/') + QString::fromUtf8(path));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(file.readAll());
    }
    static QJSValue function(QJSEngine& js, const QString& text, const QString& name)
    {
        const QString header = QStringLiteral("function ") + name + QLatin1Char('(');
        const auto start = text.indexOf(header);
        if (start < 0) return {};
        const auto line = text.lastIndexOf(QLatin1Char('\n'), start) + 1;
        const auto indent = start - line;
        const auto end = text.indexOf(QStringLiteral("\n") + QString(indent, QLatin1Char(' ')) + QLatin1Char('}'), start);
        if (end < 0) return {};
        return js.evaluate(QLatin1Char('(') + text.mid(start, end + indent + 2 - start) + QLatin1Char(')'));
    }
private slots:
    void mainSidebarSkipsFoldedRows()
    {
        QJSEngine js;
        auto setup = js.evaluate(QStringLiteral(
            "var app={games:[{},{}]}; var sounds={play:function(){}};"
            "var window={sidebarCategories:[{},{},{}],toolsCollapsed:false,sidebarHoverIndex:0};"));
        QVERIFY(!setup.isError());
        const auto main = source("src/ui/qml/Main.qml");
        auto window = js.globalObject().property("window");
        for (const auto& name : {"toolsToggleIndex", "sidebarFlatCount", "isFoldedToolRow", "sidebarStepVertical"}) {
            auto fn = function(js, main, QString::fromUtf8(name));
            QVERIFY2(fn.isCallable(), name);
            window.setProperty(QString::fromUtf8(name), fn);
        }
        QCOMPARE(js.evaluate("window.sidebarFlatCount()").toInt(), 12);
        window.setProperty("sidebarHoverIndex", 5);
        for (int expected : {6, 7, 8, 9, 10, 11, 0}) {
            QVERIFY(!js.evaluate("window.sidebarStepVertical(1)").isError());
            QCOMPARE(window.property("sidebarHoverIndex").toInt(), expected);
        }
        window.setProperty("toolsCollapsed", true);
        window.setProperty("sidebarHoverIndex", 5);
        for (int expected : {6, 10, 0}) {
            js.evaluate("window.sidebarStepVertical(1)");
            QCOMPARE(window.property("sidebarHoverIndex").toInt(), expected);
        }
        window.setProperty("sidebarHoverIndex", 0);
        js.evaluate("window.sidebarStepVertical(-1)");
        QCOMPARE(window.property("sidebarHoverIndex").toInt(), 10);
    }
    void languagePopupOwnsPadInput()
    {
        QJSEngine js;
        js.evaluate(QStringLiteral(
            "var events=[]; var window={sidebarLanguageListOpen:function(){return true;},"
            "closeSidebarLanguageList:function(){events.push('close');}};"
            "var desktopSidebar={languageCombo:{padStep:function(d){events.push(d);},"
            "padCommitHighlighted:function(){events.push('commit');}}}; var sounds={play:function(){}};"));
        const auto main = source("src/ui/qml/Main.qml");
        for (const auto& name : {"padTabStep", "padNavigate", "padNavigateVertical", "padConfirm", "padBack"}) {
            auto fn = function(js, main, QString::fromUtf8(name));
            QVERIFY2(fn.isCallable(), name);
            js.globalObject().property("window").setProperty(QString::fromUtf8(name), fn);
        }
        // Undefined background collaborators deliberately make any leaked input fail.
        for (const auto& call : {"padTabStep(-1)", "padTabStep(1)", "padNavigate(-1)", "padNavigate(1)",
                                 "padNavigateVertical(-1)", "padNavigateVertical(1)", "padConfirm()", "padBack()"}) {
            auto result = js.evaluate(QStringLiteral("window.") + QString::fromUtf8(call));
            QVERIFY2(!result.isError(), qPrintable(result.toString()));
        }
        QCOMPARE(js.evaluate("JSON.stringify(events)").toString(), QStringLiteral("[-1,1,\"commit\",\"close\"]"));
    }
    void layoutValuesAreBounded()
    {
        QJSEngine js;
        auto clamp = function(js, source("src/ui/qml/OverlayWindow.qml"), QStringLiteral("clampStep"));
        QVERIFY(clamp.isCallable());
        QCOMPARE(clamp.call({-100, 0, 96, 8, 48}).toInt(), 0);
        QCOMPARE(clamp.call({1000, 0, 96, 8, 48}).toInt(), 96);
        QCOMPARE(clamp.call({15, 0, 96, 8, 48}).toInt(), 16);
        QCOMPARE(clamp.call({js.evaluate("NaN"), 0, 96, 8, 48}).toInt(), 48);
        QCOMPARE(clamp.call({js.evaluate("Infinity"), 70, 140, 10, 100}).toInt(), 100);
        QCOMPARE(clamp.call({0, 70, 140, 10, 100}).toInt(), 70);
        auto savedWidth = function(js, source("src/ui/qml/components/DesktopSidebar.qml"), QStringLiteral("savedWidth"));
        QVERIFY(savedWidth.isCallable());
        const auto theme = source("src/ui/qml/Theme.qml");
        auto themeValue = [&theme](const QString& key) {
            return QRegularExpression(QStringLiteral("property int ") + key + QStringLiteral(": (\\d+)"))
                .match(theme).captured(1).toInt();
        };
        const int width = themeValue(QStringLiteral("sidebarWidth"));
        const int minimum = themeValue(QStringLiteral("sidebarMinWidth"));
        const int maximum = themeValue(QStringLiteral("sidebarMaxWidth"));
        QVERIFY(width > 0 && minimum > 0 && maximum >= width);
        js.evaluate(QStringLiteral("var Theme={sidebarWidth:%1,sidebarMinWidth:%2,sidebarMaxWidth:%3};"
                                   "var value; var app={config:function(){return value;}};")
                        .arg(width).arg(minimum).arg(maximum));
        for (const auto& row : {QStringLiteral("value='broken'"), QStringLiteral("value=NaN")}) {
            js.evaluate(row);
            QCOMPARE(savedWidth.call().toInt(), width);
        }
        js.evaluate("value=-1"); QCOMPARE(savedWidth.call().toInt(), minimum);
        js.evaluate("value=10000"); QCOMPARE(savedWidth.call().toInt(), maximum);
    }
    void resetRestoresSidebarAfterInteraction()
    {
        QJSEngine js;
        js.evaluate("var prefs={}; var app={games:[],config:function(k,d){return k in prefs?prefs[k]:d;}};"
                    "var closed=false; var window={sidebarCategories:[],sidebarMode:'collapsed',"
                    "toolsCollapsed:true,sidebarHoverIndex:0,sidebarLanguageListOpen:function(){return true;},"
                    "closeSidebarLanguageList:function(){closed=true;}};");
        const auto main = source("src/ui/qml/Main.qml");
        auto window = js.globalObject().property("window");
        for (const auto& name : {"normalizedSidebarMode", "toolsToggleIndex", "isFoldedToolRow", "refreshSidebarPreferences"}) {
            auto fn = function(js, main, QString::fromUtf8(name));
            QVERIFY2(fn.isCallable(), name);
            window.setProperty(QString::fromUtf8(name), fn);
        }
        QVERIFY(!js.evaluate("window.refreshSidebarPreferences()").isError());
        QCOMPARE(window.property("sidebarMode").toString(), QStringLiteral("expanded"));
        QVERIFY(!window.property("toolsCollapsed").toBool());
        js.evaluate("prefs={'ui.main_sidebar_mode':'broken','ui.main_tools_collapsed':'true'}; window.sidebarHoverIndex=6;");
        QVERIFY(!js.evaluate("window.refreshSidebarPreferences()").isError());
        QCOMPARE(window.property("sidebarMode").toString(), QStringLiteral("expanded"));
        QCOMPARE(window.property("sidebarHoverIndex").toInt(), 0);
        QVERIFY(js.globalObject().property("closed").toBool());
    }
    void allOverlayPanelsBlockQueuedPlayback()
    {
        QJSEngine js;
        js.evaluate("var plays=0; var overlayShare={isOpen:false}; var viewer={open:false};"
                    "var content={menuOpen:false,layoutPanelOpen:false,toggleVideoPlayback:function(){++plays;}};");
        auto toggle = function(js, source("src/ui/qml/OverlayWindow.qml"), QStringLiteral("togglePlayback"));
        QVERIFY(toggle.isCallable());
        for (const auto& state : {"content.menuOpen=true", "content.menuOpen=false; content.layoutPanelOpen=true",
                                  "content.layoutPanelOpen=false; overlayShare.isOpen=true"}) {
            js.evaluate(QString::fromUtf8(state));
            QVERIFY(!toggle.call().isError());
            QCOMPARE(js.globalObject().property("plays").toInt(), 0);
        }
        js.evaluate("overlayShare.isOpen=false");
        // Closing a panel does not resume anything; the next explicit command works.
        QCOMPARE(js.globalObject().property("plays").toInt(), 0);
        QVERIFY(!toggle.call().isError());
        QCOMPARE(js.globalObject().property("plays").toInt(), 1);
    }
};
QTEST_GUILESS_MAIN(RecentUiTest)
#include "tst_recentui.moc"
