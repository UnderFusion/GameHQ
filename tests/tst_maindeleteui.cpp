// Focused harness: extract Main.qml input and bulk-delete handlers into a
// synthetic window with the real ConfirmDialog, BulkSelection and
// DesktopPadRouter (which routes the controller signals). This is not a
// full Main.qml integration test. The application/gallery boundary is stubbed;
// delete effects use files.
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
    // The gallery as the harness sees it: one capture per path, in row order.
    Q_PROPERTY(QStringList paths MEMBER paths)
public:
    QStringList paths;
    int deletions = 0;
    int sideEffects = 0;
    Q_INVOKABLE void removeSelected()
    {
        ++deletions;
        for (const auto& path : paths)
            QFile::remove(path);
    }
    Q_INVOKABLE void deleteRows(const QVariantList& rows)
    {
        ++deletions;
        for (const auto& row : rows)
            QFile::remove(paths.value(row.toInt()));
    }
    // Favorite toggles, zoom steps, settings pages: anything a background
    // command could do behind an open confirmation.
    Q_INVOKABLE void sideEffect() { ++sideEffects; }
    Q_INVOKABLE void play(const QString&) {}
signals:
    void desktopConfirm();
    void desktopNavigate(int direction);
    void desktopNavigateVertical(int direction);
    void desktopBack();
    void desktopFavorite();
    void desktopMenu();
    void desktopTabStep(int direction);
    void desktopSettings();
    void desktopZoom(int direction);
    void desktopBulkToggle();
    // Routed by DesktopPadRouter too; declared so it connects to every handler.
    void desktopScroll(int direction);
    void playbackPlayPause();
    void playbackSeek(int direction);
    void frameGrabRequested();
};

class MainDeleteUiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
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

    QVariant call(const char* function, const QVariant& arg = {})
    {
        QVariant result;
        if (arg.isValid())
            QMetaObject::invokeMethod(m_window.get(), function, Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, arg));
        else
            QMetaObject::invokeMethod(m_window.get(), function, Q_RETURN_ARG(QVariant, result));
        return result;
    }

    void waitForCancelFocus()
    {
        // open() defers focus. A hidden previous dialog can still own a
        // button labelled Cancel, so the label alone is not readiness.
        QTRY_VERIFY(focusIsInsideDialog());
        QTRY_COMPARE(m_window->activeFocusItem()->property("label").toString(), QStringLiteral("Cancel"));
    }

    // Bulk opens the way production does: select in bulk mode, then ask.
    void openBulk(const QVariantList& rows)
    {
        m_dialog = m_window->findChild<QObject*>("bulk");
        QVERIFY(m_dialog);
        m_window->setProperty("bulkMode", true);
        call("bulkClear");
        for (const auto& row : rows)
            QMetaObject::invokeMethod(m_window.get(), "bulkToggle", Q_ARG(QVariant, row),
                                      Q_ARG(QVariant, false));
        QCOMPARE(call("bulkCount").toInt(), rows.size());
        call("bulkAskDelete");
        QVERIFY(m_dialog->property("visible").toBool());
        waitForCancelFocus();
    }

    void openDialog(bool bulk)
    {
        if (bulk) {
            openBulk({0, 1});
            return;
        }
        m_dialog = m_window->findChild<QObject*>("single");
        QVERIFY(m_dialog);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "open"));
        waitForCancelFocus();
    }

    void verifyFiles(bool exist)
    {
        for (const auto& path : m_boundary.paths)
            QCOMPARE(QFile::exists(path), exist);
    }

    QString addCapture(const QString& name)
    {
        const auto path = m_dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write("capture") <= 0)
            return {};
        return path;
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
    property int menuIndex: 0
    property bool helpOpen: false
    property var pendingBulkDeletePaths: []
    function sidebarLanguageListOpen() { return false }
    function activeShareDialog() { return null }
    function focusGalleryOrSidebar() {}
    function zoomIn() { boundary.sideEffect() }
    function zoomOut() { boundary.sideEffect() }
    function toggleSidebarGamePin(index) { boundary.sideEffect() }
    QtObject {
        id: app
        property QtObject gallery: QtObject {
            function rowCount() { return boundary.paths.length }
            function get(row) { return { filePath: boundary.paths[row] } }
            function rowOf(path) { return boundary.paths.indexOf(path) }
        }
        function deleteCaptures(rows) { boundary.deleteRows(rows) }
        function toggleFavorite(row) { boundary.sideEffect() }
    }
    BulkSelection { id: bulkSelection; galleryModel: app.gallery }
    // Any selection path that might bypass the window gates.
    function selectAllBehindGates() { bulkSelection.selectAll() }
    Item { id: grid; property int count: 1; property int currentIndex: 0 }
    Item { id: settingsView; function padCategoryStep(direction) { boundary.sideEffect() } }
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
        onCanceled: window.pendingBulkDeletePaths = []
    }
)qml";
        for (const auto& name : {"padNavigate", "padNavigateVertical", "padConfirm", "padBack",
                                 "activeConfirmDialog", "confirmDialogOpen", "activeModal",
                                 "bulkCount", "bulkIsChecked", "bulkAllSelected",
                                 "bulkToggle", "bulkClear", "bulkEnter", "bulkExit", "openSettings",
                                 "padBulkToggle", "bulkSelectAll", "bulkRows", "bulkAskDelete",
                                 "bulkConfirmDelete", "padTabStep", "padFavorite", "padToggleMenu"}) {
            const QString function = productionFunction(source, QString::fromUtf8(name));
            QVERIFY2(!function.isEmpty(), name);
            harness += function.toUtf8() + '\n';
        }
        // The production router, wired the way Main.qml wires it.
        QVERIFY(source.contains(QStringLiteral("DesktopPadRouter {")));
        harness += "DesktopPadRouter { target: boundary; host: window; viewer: lightbox }\n}";
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
        m_boundary.sideEffects = 0;
        for (int i = 0; i < 2; ++i) {
            const auto path = addCapture(QStringLiteral("capture-%1.jpg").arg(i));
            QVERIFY(!path.isEmpty());
            m_boundary.paths.append(path);
        }
    }

    void cleanup()
    {
        for (const auto* name : {"single", "bulk"})
            QVERIFY(QMetaObject::invokeMethod(m_window->findChild<QObject*>(name), "close"));
        m_window->setProperty("bulkMode", false);
        m_window->setProperty("settingsOpen", false);
        m_window->setProperty("menuOpen", false);
        call("bulkClear");
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

    // aud-a10: Triangle (select all) and every other background command are
    // dropped while the bulk confirmation is open; it deletes what it listed.
    void backgroundCommandsCannotWidenBulkDelete()
    {
        openBulk({0});
        emit m_boundary.desktopFavorite();
        emit m_boundary.desktopMenu();
        emit m_boundary.desktopTabStep(1);
        emit m_boundary.desktopSettings();
        emit m_boundary.desktopZoom(1);
        emit m_boundary.desktopBulkToggle();
        // Mouse and keyboard reach the selection through the same window API.
        call("bulkSelectAll");
        QMetaObject::invokeMethod(m_window.get(), "bulkToggle", Q_ARG(QVariant, 1), Q_ARG(QVariant, false));
        QVERIFY(m_dialog->property("visible").toBool());
        QVERIFY(focusIsInsideDialog());
        QCOMPARE(call("bulkCount").toInt(), 1);
        QVERIFY(m_window->property("bulkMode").toBool());
        QVERIFY(!m_window->property("settingsOpen").toBool());
        QVERIFY(!m_window->property("menuOpen").toBool());
        QCOMPARE(m_boundary.sideEffects, 0);

        emit m_boundary.desktopNavigate(1);
        QTRY_COMPARE(m_window->activeFocusItem()->property("label").toString(), QStringLiteral("Delete"));
        emit m_boundary.desktopConfirm();
        QCOMPARE(m_boundary.deletions, 1);
        QVERIFY(!QFile::exists(m_boundary.paths.at(0)));
        QVERIFY(QFile::exists(m_boundary.paths.at(1)));
    }

    // A gallery refresh behind the dialog shifts rows and drops a listed file,
    // and the selection widens past the gates: confirm still deletes only the
    // listed files that remain, found by path.
    void galleryRefreshKeepsFrozenTargets()
    {
        const auto extra = addCapture(QStringLiteral("capture-2.jpg"));
        QVERIFY(!extra.isEmpty());
        m_boundary.paths.append(extra);
        openBulk({1, 2});
        const auto listed = m_boundary.paths.at(1);
        const auto removed = m_boundary.paths.at(2);
        const auto newcomer = addCapture(QStringLiteral("capture-new.jpg"));
        QVERIFY(!newcomer.isEmpty());
        m_boundary.paths = {newcomer, m_boundary.paths.at(0), listed};
        call("selectAllBehindGates");
        QCOMPARE(call("bulkCount").toInt(), 3);

        emit m_boundary.desktopNavigate(1);
        QTRY_COMPARE(m_window->activeFocusItem()->property("label").toString(), QStringLiteral("Delete"));
        emit m_boundary.desktopConfirm();
        QCOMPARE(m_boundary.deletions, 1);
        QVERIFY(!QFile::exists(listed));
        QVERIFY(QFile::exists(newcomer));
        QVERIFY(QFile::exists(m_boundary.paths.at(1)));
        QVERIFY(QFile::exists(removed));
    }
};

QTEST_MAIN(MainDeleteUiTest)
#include "tst_maindeleteui.moc"
