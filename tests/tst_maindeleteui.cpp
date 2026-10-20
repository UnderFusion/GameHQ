// Focused harness: extract Main.qml input handlers into a synthetic window
// with the real ConfirmDialog. This is not a full Main.qml integration test.
// The application/gallery boundary is stubbed; delete effects use files.
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

class DeleteBoundary : public QObject
{
    Q_OBJECT
public:
    QStringList paths;
    int deletions = 0;
    Q_INVOKABLE void removeSelected()
    {
        ++deletions;
        for (const auto& path : paths)
            QFile::remove(path);
    }
    Q_INVOKABLE void play(const QString&) {}
signals:
    void desktopConfirm();
    void desktopNavigate(int direction);
    void desktopNavigateVertical(int direction);
    void desktopBack();
};

class MainDeleteUiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir{QStringLiteral(GAMEHQ_SOURCE_DIR "/.claude-gui-temp/maindelete-XXXXXX")};
    DeleteBoundary m_boundary;
    QQmlEngine m_engine;
    std::unique_ptr<QQuickWindow> m_window;
    QObject* m_dialog = nullptr;

    static QString productionFunction(const QString& source, const QString& name)
    {
        const QString header = QStringLiteral("function ") + name + QLatin1Char('(');
        const auto start = source.indexOf(header);
        if (start < 0)
            return {};
        const auto line = source.lastIndexOf(QLatin1Char('\n'), start) + 1;
        const auto indent = start - line;
        const auto end = source.indexOf(QLatin1Char('\n') + QString(indent, QLatin1Char(' '))
                                           + QLatin1Char('}'), start);
        return end < 0 ? QString() : source.mid(start, end + indent + 2 - start);
    }

    bool focusIsInsideDialog() const
    {
        for (auto* item = m_window->activeFocusItem(); item; item = item->parentItem()) {
            if (item == m_dialog)
                return true;
        }
        return false;
    }

    void openDialog(bool bulk)
    {
        m_dialog = m_window->findChild<QObject*>(bulk ? "bulk" : "single");
        QVERIFY(m_dialog);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "open"));
        // open() defers focus. A hidden previous dialog can still own a
        // button labelled Cancel, so the label alone is not readiness.
        QTRY_VERIFY(focusIsInsideDialog());
        QTRY_COMPARE(m_window->activeFocusItem()->property("label").toString(), QStringLiteral("Cancel"));
    }

    void verifyFiles(bool exist)
    {
        for (const auto& path : m_boundary.paths)
            QCOMPARE(QFile::exists(path), exist);
    }

    static void dialogRows()
    {
        QTest::addColumn<bool>("bulk");
        QTest::newRow("single") << false;
        QTest::newRow("bulk") << true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QFile main(QStringLiteral(GAMEHQ_SOURCE_DIR "/src/ui/qml/Main.qml"));
        QVERIFY(main.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(main.readAll());
        QByteArray harness = R"qml(
import QtQuick
import "../src/ui/qml/components"
Window {
    id: window
    width: 900; height: 600
    visible: true
    property bool settingsOpen: false
    property bool usingGamepad: false
    property bool sidebarFocused: false
    property bool bulkMode: false
    property bool menuOpen: false
    property bool helpOpen: false
    function sidebarLanguageListOpen() { return false }
    function activeShareDialog() { return null }
    function bulkConfirmDelete() { boundary.removeSelected() }
    Item { id: helpDialog; visible: false }
    Item { id: aboutDialog; visible: false }
    Item { id: lightbox; visible: false }
    ConfirmDialog {
        id: deleteDialog; objectName: "single"; anchors.fill: parent
        cancelLabel: "Cancel"; confirmLabel: "Delete"
        onConfirmed: boundary.removeSelected()
    }
    ConfirmDialog {
        id: bulkDeleteDialog; objectName: "bulk"; anchors.fill: parent
        cancelLabel: "Cancel"; confirmLabel: "Delete"
        onConfirmed: window.bulkConfirmDelete()
    }
)qml";
        for (const auto& name : {"padNavigate", "padNavigateVertical", "padConfirm", "padBack"}) {
            const QString function = productionFunction(source, QString::fromUtf8(name));
            QVERIFY2(!function.isEmpty(), name);
            harness += function.toUtf8() + '\n';
        }
        harness += "Connections { target: boundary\n";
        for (const auto& name : {"onDesktopNavigate", "onDesktopNavigateVertical", "onDesktopConfirm", "onDesktopBack"}) {
            const QString function = productionFunction(source, QString::fromUtf8(name));
            QVERIFY2(!function.isEmpty(), name);
            harness += function.toUtf8() + '\n';
        }
        harness += "}\n}";
        m_engine.addImportPath(QStringLiteral(GAMEHQ_SOURCE_DIR "/tests/qml/imports"));
        m_engine.rootContext()->setContextProperty(QStringLiteral("boundary"), &m_boundary);
        m_engine.rootContext()->setContextProperty(QStringLiteral("sounds"), &m_boundary);
        QQmlComponent component(&m_engine);
        component.setData(harness, QUrl::fromLocalFile(QStringLiteral(GAMEHQ_SOURCE_DIR "/tests/main-delete-harness.qml")));
        m_window.reset(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY2(m_window, qPrintable(component.errorString()));
        m_window->requestActivate();
        QTRY_VERIFY(m_window->isActive());
    }

    void init()
    {
        m_boundary.paths.clear();
        m_boundary.deletions = 0;
        for (int i = 0; i < 2; ++i) {
            const auto path = m_dir.filePath(QStringLiteral("capture-%1.jpg").arg(i));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("capture") > 0);
            m_boundary.paths.append(path);
        }
    }

    void cleanup()
    {
        for (const auto* name : {"single", "bulk"})
            QVERIFY(QMetaObject::invokeMethod(m_window->findChild<QObject*>(name), "close"));
        m_dialog = nullptr;
    }

    void crossOnCancelKeepsFiles_data() { dialogRows(); }
    void crossOnCancelKeepsFiles()
    {
        QFETCH(bool, bulk);
        openDialog(bulk);
        emit m_boundary.desktopConfirm();
        QVERIFY(!m_dialog->property("visible").toBool());
        QCOMPARE(m_boundary.deletions, 0);
        verifyFiles(true);
    }

    void deliberateNavigationDeletesOnce_data()
    {
        QTest::addColumn<bool>("bulk");
        QTest::addColumn<bool>("vertical");
        QTest::newRow("single-right") << false << false;
        QTest::newRow("bulk-right") << true << false;
        QTest::newRow("single-down") << false << true;
        QTest::newRow("bulk-down") << true << true;
    }
    void deliberateNavigationDeletesOnce()
    {
        QFETCH(bool, bulk);
        QFETCH(bool, vertical);
        openDialog(bulk);
        if (vertical)
            emit m_boundary.desktopNavigateVertical(1);
        else
            emit m_boundary.desktopNavigate(1);
        QTRY_COMPARE(m_window->activeFocusItem()->property("label").toString(), QStringLiteral("Delete"));
        emit m_boundary.desktopConfirm();
        QVERIFY(!m_dialog->property("visible").toBool());
        QCOMPARE(m_boundary.deletions, 1);
        verifyFiles(false);
        // Re-entering starts safely at Cancel even after a confirmed delete.
        openDialog(bulk);
        emit m_boundary.desktopConfirm();
        QCOMPARE(m_boundary.deletions, 1);
    }

    void returnToCancelKeepsFiles_data() { dialogRows(); }
    void returnToCancelKeepsFiles()
    {
        QFETCH(bool, bulk);
        openDialog(bulk);
        emit m_boundary.desktopNavigate(1);
        emit m_boundary.desktopNavigate(-1);
        QCOMPARE(m_window->activeFocusItem()->property("label").toString(), QStringLiteral("Cancel"));
        emit m_boundary.desktopConfirm();
        QCOMPARE(m_boundary.deletions, 0);
        verifyFiles(true);
    }

    void circleCancelsFocusedDelete_data() { dialogRows(); }
    void circleCancelsFocusedDelete()
    {
        QFETCH(bool, bulk);
        openDialog(bulk);
        emit m_boundary.desktopNavigate(1);
        QCOMPARE(m_window->activeFocusItem()->property("label").toString(), QStringLiteral("Delete"));
        emit m_boundary.desktopBack();
        QVERIFY(!m_dialog->property("visible").toBool());
        QCOMPARE(m_boundary.deletions, 0);
        verifyFiles(true);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    MainDeleteUiTest test;
    auto arguments = app.arguments();
    arguments << QStringLiteral("-o")
              << QStringLiteral(GAMEHQ_SOURCE_DIR "/.claude-gui-temp/maindeleteui-results.txt,txt")
              << QStringLiteral("-o") << QStringLiteral("-,txt");
    return QTest::qExec(&test, arguments);
}
#include "tst_maindeleteui.moc"
