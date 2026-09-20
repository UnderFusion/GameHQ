// Drives the REAL MappingPresetSection.qml against the real MappingPresetModel.
//
// The cpo-p06 acceptance review found the section's assignment picker dead in
// QML: rebuildOptions() built its list and returned it instead of assigning the
// property the control is bound to, so the picker stayed empty and never showed
// the assignment. That bug is invisible to the C++ model suites, so this suite
// loads the shipped QML file, wires the same `input.mappingPresets` the page
// uses, and asserts what the controls actually contain.

#include "input/MappingPresetModel.h"
#include "input/MappingAssignmentResolver.h"
#include "storage/CaptureDatabase.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QScopedPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

namespace
{
// The page's `input` object, reduced to the one property the section uses.
class StubInput : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *mappingPresets READ mappingPresets CONSTANT)
public:
    explicit StubInput(QObject *presets, QObject *parent = nullptr)
        : QObject(parent)
        , m_presets(presets)
    {
    }
    QObject *mappingPresets() const { return m_presets; }

private:
    QObject *m_presets = nullptr;
};
} // namespace

class MappingPresetSectionTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QQmlEngine m_engine;
    QScopedPointer<CaptureDatabase> m_database;
    QScopedPointer<MappingPresetModel> m_model;
    QScopedPointer<QObject> m_section;
    QUrl m_sectionUrl;
    QString m_dbPath;
    int m_sequence = 0;

    QObject *loadSection();
    QObject *combo(const char *objectName) const;
    QString firstPresetId() const;
    // The session game this suite simulates: one canonical key, so the raw row
    // written below and the model's game target cannot disagree.
    void applyRacerGameTarget();
    QString racerGameKey() const;
    // Storage refuses a cross-group game assignment - it is only reachable through
    // a database damaged outside the app - so that fixture row is written out of
    // band, the same approach as tst_gamesessionpresets.
    bool rawExec(const QString& sql);

private slots:
    void initTestCase();
    void init();
    void assignmentPickerIsPopulatedAndFollowsTheLibrary();
    void libraryPickerTracksLibraryMutations();
    void librarySelectionDoesNotChangeAssignmentOrRuntime();
    void deleteDialogRefusesWhatStorageWouldRefuse();
    void gameRowFollowsTheSessionAndTheModel();
    void gameRowNamesAWrongGroupAssignment();
};

void MappingPresetSectionTest::initTestCase()
{
    // The GameHQ module in the build tree carries `prefer :/qt/qml/GameHQ/`, so
    // its components resolve their relative imports (helpers/*.js) against the
    // resource copy the app embeds — which a test binary does not have. Copy the
    // manifest without that line, with the QML sources beside it, and import the
    // module from there: the suite then exercises the real files.
    QFile manifest(QStringLiteral(GAMEHQ_QML_IMPORT_DIR "/GameHQ/qmldir"));
    QVERIFY(manifest.open(QIODevice::ReadOnly | QIODevice::Text));
    QStringList entries;
    while (!manifest.atEnd()) {
        const QString line = QString::fromUtf8(manifest.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("prefer ")))
            continue;
        entries.append(line);
    }
    manifest.close();
    QVERIFY(entries.size() > 10);

    const QString moduleRoot = m_dir.filePath(QStringLiteral("qmltest"));
    const QString moduleDir = moduleRoot + QStringLiteral("/GameHQ");
    QVERIFY(QDir().mkpath(moduleDir));

    // ui/qml/** of the source tree, mirrored under the module's ui/ directory so
    // the qmldir entries (relative to the module root) resolve.
    QDirIterator it(QStringLiteral(GAMEHQ_QML_SOURCE_DIR "/ui"),
                    QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QString sourceRoot = QStringLiteral(GAMEHQ_QML_SOURCE_DIR "/ui");
    int copied = 0;
    while (it.hasNext()) {
        const QString source = it.next();
        const QString relative = QDir(sourceRoot).relativeFilePath(source);
        const QString target = moduleDir + QStringLiteral("/ui/") + relative;
        QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
        QVERIFY2(QFile::copy(source, target), qPrintable(target));
        ++copied;
    }
    QVERIFY(copied > 50);

    QFile generated(moduleDir + QStringLiteral("/qmldir"));
    QVERIFY(generated.open(QIODevice::WriteOnly | QIODevice::Text));
    generated.write(entries.join(QLatin1Char('\n')).toUtf8());
    generated.write("\n");
    generated.close();

    m_engine.addImportPath(moduleRoot);
    m_sectionUrl = QUrl::fromLocalFile(moduleDir
                                       + QStringLiteral("/ui/qml/settings/MappingPresetSection.qml"));
}

void MappingPresetSectionTest::init()
{
    m_section.reset();
    m_model.reset();
    m_database.reset();

    // A fresh library per case: every case mutates presets and assignments.
    const QString file = m_dir.filePath(QStringLiteral("section_%1.db").arg(++m_sequence));
    m_dbPath = file;
    m_database.reset(new CaptureDatabase(file, nullptr));
    QVERIFY(m_database->open());
    m_model.reset(new MappingPresetModel(m_database.data()));

    // The page's context objects must outlive the QML they feed.
    auto *input = new StubInput(m_model.data(), &m_engine);
    m_engine.rootContext()->setContextProperty(QStringLiteral("input"), input);
    auto *app = new QObject(&m_engine);
    m_engine.rootContext()->setContextProperty(QStringLiteral("app"), app);
    auto *sounds = new QObject(&m_engine);
    m_engine.rootContext()->setContextProperty(QStringLiteral("sounds"), sounds);

    m_section.reset(loadSection());
    QVERIFY(m_section);
}

QObject *MappingPresetSectionTest::loadSection()
{
    QQmlComponent component(&m_engine, m_sectionUrl);
    QObject *object = component.create();
    if (!object)
        qWarning().noquote() << component.errorString();
    return object;
}

QObject *MappingPresetSectionTest::combo(const char *objectName) const
{
    return m_section->findChild<QObject *>(QString::fromLatin1(objectName));
}

QString MappingPresetSectionTest::firstPresetId() const
{
    const QVariantList presets = m_model->presets();
    return presets.isEmpty()
               ? QString()
               : presets.first().toMap().value(QStringLiteral("id")).toString();
}

void MappingPresetSectionTest::applyRacerGameTarget()
{
    m_model->setGameTargetProvider([] {
        MappingPresetModel::GameTarget target;
        target.key = MappingAssignmentResolver::canonicalGameKey(
            QStringLiteral("C:\\Games\\Racer\\Racer.exe"));
        target.label = QStringLiteral("Racer");
        return target;
    });
    m_model->refreshGameTarget();
}

QString MappingPresetSectionTest::racerGameKey() const
{
    return MappingAssignmentResolver::canonicalGameKey(
        QStringLiteral("C:\\Games\\Racer\\Racer.exe"));
}

bool MappingPresetSectionTest::rawExec(const QString& sql)
{
    bool ok = false;
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("section-raw"));
        raw.setDatabaseName(m_dbPath);
        if (raw.open()) {
            QSqlQuery query(raw);
            ok = query.exec(sql);
            raw.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("section-raw"));
    return ok;
}

void MappingPresetSectionTest::assignmentPickerIsPopulatedAndFollowsTheLibrary()
{
    QObject *assignmentCombo = combo("presetAssignmentCombo");
    QVERIFY(assignmentCombo);

    // Follow fallback + Built-in defaults, before any preset exists.
    QVariantList options = m_section->property("assignmentOptions").toList();
    QCOMPARE(options.size(), 2);
    QCOMPARE(options.at(0).toMap().value(QStringLiteral("value")).toString(), QString());
    QCOMPARE(options.at(1).toMap().value(QStringLiteral("value")).toString(),
             QStringLiteral("@builtin"));
    // The control really received them: this is the assertion the pre-fix code
    // failed, because the list never reached the bound property.
    QCOMPARE(assignmentCombo->property("count").toInt(), 2);
    QCOMPARE(assignmentCombo->property("currentIndex").toInt(), 0);

    // A library mutation refreshes the picker.
    QVERIFY(m_model->createPreset(QStringLiteral("Desk pad")));
    QCOMPARE(m_section->property("assignmentOptions").toList().size(), 3);
    QCOMPARE(assignmentCombo->property("count").toInt(), 3);

    // Assigning it moves the picker (and the model agrees).
    const QString presetId = m_model->selectedPresetId();
    QVERIFY(!presetId.isEmpty());
    QVERIFY(m_model->applyAssignment(presetId));
    QCOMPARE(m_model->assignedPresetId(), presetId);
    const QVariantList afterAssign = m_section->property("assignmentOptions").toList();
    int assignedIndex = -1;
    for (int i = 0; i < afterAssign.size(); ++i)
        if (afterAssign.at(i).toMap().value(QStringLiteral("value")).toString() == presetId)
            assignedIndex = i;
    QCOMPARE(assignedIndex, 2);
    QCOMPARE(assignmentCombo->property("currentIndex").toInt(), assignedIndex);
}

void MappingPresetSectionTest::libraryPickerTracksLibraryMutations()
{
    QObject *libraryCombo = combo("presetLibraryCombo");
    QVERIFY(libraryCombo);
    QCOMPARE(m_section->property("libraryOptions").toList().size(), 0);

    QVERIFY(m_model->createPreset(QStringLiteral("First")));
    QVariantList options = m_section->property("libraryOptions").toList();
    QCOMPARE(options.size(), 1);
    QCOMPARE(options.first().toMap().value(QStringLiteral("label")).toString(),
             QStringLiteral("First"));
    QCOMPARE(libraryCombo->property("count").toInt(), 1);

    // A rename is a library-only change: the picker follows it, the assignment
    // (still "follow fallback") does not move.
    QVERIFY(m_model->renameSelected(QStringLiteral("Renamed")));
    options = m_section->property("libraryOptions").toList();
    QCOMPARE(options.size(), 1);
    QCOMPARE(options.first().toMap().value(QStringLiteral("label")).toString(),
             QStringLiteral("Renamed"));
    QVERIFY(m_model->assignedToFallback());
    // The picker selected the preset, so the section can rename/duplicate/delete
    // it without touching the device assignment.
    QCOMPARE(m_model->selectedPresetId(), firstPresetId());
}

void MappingPresetSectionTest::librarySelectionDoesNotChangeAssignmentOrRuntime()
{
    QVERIFY(m_model->createPreset(QStringLiteral("Pad A")));
    const QString first = m_model->selectedPresetId();
    QVERIFY(m_model->createPreset(QStringLiteral("Pad B")));
    QString second;
    for (const QVariant &entry : m_model->presets()) {
        const QString id = entry.toMap().value(QStringLiteral("id")).toString();
        if (id != first)
            second = id;
    }
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(second != first);

    int runtimeRefreshes = 0;
    m_model->setRuntimeRefresh([&runtimeRefreshes] { ++runtimeRefreshes; });

    QVERIFY(m_model->applyAssignment(first));
    QCOMPARE(m_model->assignedPresetId(), first);
    const int refreshesAfterAssign = runtimeRefreshes;
    QCOMPARE(refreshesAfterAssign, 1);

    // Select the OTHER preset through the real control, exactly as a click does.
    QObject *libraryCombo = combo("presetLibraryCombo");
    QVERIFY(libraryCombo);
    const QVariantList options = m_section->property("libraryOptions").toList();
    int secondIndex = -1;
    for (int i = 0; i < options.size(); ++i)
        if (options.at(i).toMap().value(QStringLiteral("value")).toString() == second)
            secondIndex = i;
    QVERIFY(secondIndex >= 0);
    QVERIFY(QMetaObject::invokeMethod(libraryCombo, "commit", Q_ARG(QVariant, secondIndex)));

    QCOMPARE(m_model->selectedPresetId(), second);
    // ...and that is all it did: same assignment, no runtime invalidation.
    QCOMPARE(m_model->assignedPresetId(), first);
    QCOMPARE(runtimeRefreshes, refreshesAfterAssign);

    // A refused switch (an open draft) leaves the model where it was, and the
    // control snaps back to the model instead of showing the refused choice.
    m_model->setPendingEditProvider([] { return true; });
    int firstIndex = -1;
    for (int i = 0; i < options.size(); ++i)
        if (options.at(i).toMap().value(QStringLiteral("value")).toString() == first)
            firstIndex = i;
    QVERIFY(firstIndex >= 0);
    QVERIFY(QMetaObject::invokeMethod(libraryCombo, "commit", Q_ARG(QVariant, firstIndex)));
    QCOMPARE(m_model->selectedPresetId(), second);
    QCOMPARE(runtimeRefreshes, refreshesAfterAssign);
    // Qt.callLater refreshes on the next event loop pass.
    QTRY_COMPARE(libraryCombo->property("currentIndex").toInt(), secondIndex);
}

void MappingPresetSectionTest::gameRowFollowsTheSessionAndTheModel()
{
    QObject *gameCombo = combo("presetGameCombo");
    QVERIFY(gameCombo);

    // No game in session: only the two virtual choices exist, and a commit is
    // refused (there is no game key to write a row for).
    QVERIFY(!m_model->gameAvailable());
    QCOMPARE(m_section->property("gameOptions").toList().size(), 2);
    QCOMPARE(gameCombo->property("count").toInt(), 2);
    QVERIFY(QMetaObject::invokeMethod(gameCombo, "commit", Q_ARG(QVariant, 0)));
    QCOMPARE(m_model->noticeKind(), QStringLiteral("game_unavailable"));
    m_model->clearNotice();

    // The session game arrives: the model publishes it, the picker is rebuilt,
    // and the row still follows the chain below the game.
    m_model->setGameTargetProvider([] {
        MappingPresetModel::GameTarget target;
        target.key = MappingAssignmentResolver::canonicalGameKey(
            QStringLiteral("C:\\Games\\Racer\\Racer.exe"));
        target.label = QStringLiteral("Racer");
        return target;
    });
    m_model->refreshGameTarget();
    QVERIFY(m_model->gameAvailable());
    QTRY_COMPARE(m_section->property("gameOptions").toList().size(), 2);
    QTRY_COMPARE(gameCombo->property("currentIndex").toInt(), 0);

    // Assigning through the real control writes the game row and nothing else.
    QVERIFY(m_model->createPreset(QStringLiteral("Racer preset")));
    const QString preset = firstPresetId();
    QVERIFY(!preset.isEmpty());
    int runtimeRefreshes = 0;
    m_model->setRuntimeRefresh([&runtimeRefreshes] { ++runtimeRefreshes; });
    QTRY_COMPARE(m_section->property("gameOptions").toList().size(), 3);
    QVERIFY(QMetaObject::invokeMethod(gameCombo, "commit", Q_ARG(QVariant, 2)));
    QCOMPARE(m_model->gameAssignedPresetId(), preset);
    QCOMPARE(m_model->gameAssignedPresetName(), QStringLiteral("Racer preset"));
    // The device assignment is a different question and did not move.
    QVERIFY(m_model->assignedToFallback());
    QCOMPARE(runtimeRefreshes, 1);

    // A refused write (an open draft) leaves the game row alone and the control
    // snaps back to the model instead of showing the refused choice.
    m_model->setPendingEditProvider([] { return true; });
    QVERIFY(QMetaObject::invokeMethod(gameCombo, "commit", Q_ARG(QVariant, 0)));
    QCOMPARE(m_model->gameAssignedPresetId(), preset);
    QCOMPARE(runtimeRefreshes, 1);
    QTRY_COMPARE(gameCombo->property("currentIndex").toInt(), 2);
}

void MappingPresetSectionTest::deleteDialogRefusesWhatStorageWouldRefuse()
{
    const QString dialogUrl = QString(m_sectionUrl.toString())
                                  .replace(QStringLiteral("settings/MappingPresetSection.qml"),
                                           QStringLiteral("components/MappingPresetDeleteDialog.qml"));
    QQmlComponent component(&m_engine, QUrl(dialogUrl));
    QScopedPointer<QObject> dialog(component.create());
    if (!dialog)
        qWarning().noquote() << component.errorString();
    QVERIFY(dialog);

    const QVariantList candidates{QVariantMap{{QStringLiteral("id"), QStringLiteral("p2")},
                                              {QStringLiteral("name"), QStringLiteral("Other")}}};

    // Unreferenced: a plain confirm, nothing to move.
    QVERIFY(!dialog->property("needsReassign").toBool());
    QVERIFY(!dialog->property("blocked").toBool());

    // Referenced with another preset available: pick where its users go.
    dialog->setProperty("candidates", candidates);
    dialog->setProperty("movableReferences", 1);
    QVERIFY(dialog->property("needsReassign").toBool());
    QVERIFY(!dialog->property("blocked").toBool());

    // Referenced with nothing left to move its users to: the model can only
    // refuse, so the dialog blocks instead of enabling a Delete that fails.
    dialog->setProperty("candidates", QVariantList());
    QVERIFY(!dialog->property("needsReassign").toBool());
    QVERIFY(dialog->property("blocked").toBool());

    // A migration record is never a movable set, with or without candidates.
    dialog->setProperty("movableReferences", 0);
    dialog->setProperty("migrationReferences", 1);
    dialog->setProperty("candidates", candidates);
    QVERIFY(dialog->property("blocked").toBool());
    QVERIFY(!dialog->property("needsReassign").toBool());
}

// The game row must NAME a wrong-group assignment instead of falling through to
// the normal sentence with an empty preset name (exactly what the cpo-p07 review
// found in the shipped QML). The row publishes its state, so the check does not
// depend on a translated sentence.
void MappingPresetSectionTest::gameRowNamesAWrongGroupAssignment()
{
    applyRacerGameTarget();
    QVERIFY(m_model->gameAvailable());

    QObject *gameRow = m_section->findChild<QObject *>(QStringLiteral("presetGameRow"));
    QVERIFY(gameRow);
    QVERIFY(gameRow->property("visible").toBool());

    // A real controller preset on the game: the normal state, whose sentence a
    // broken row must NOT render.
    QVERIFY(m_model->createPreset(QStringLiteral("Racer preset")));
    const QString controllerPreset = firstPresetId();
    QVERIFY(!controllerPreset.isEmpty());
    QVERIFY(m_model->applyGameAssignment(controllerPreset));
    QTRY_COMPARE(gameRow->property("assignmentState").toString(),
                 QStringLiteral("preset"));
    const QString normalDescription = gameRow->property("description").toString();
    QVERIFY(!normalDescription.isEmpty());
    QCOMPARE(gameRow->property("tone").toString(), QStringLiteral("normal"));

    // A keyboard preset for the same game. Storage refuses that pairing, so the
    // damaged row is written out of band - like a database repaired outside the app.
    m_model->setDeviceGroup(QStringLiteral("keyboard"));
    QVERIFY(m_model->createPreset(QStringLiteral("Keys")));
    const QString keyboardPreset = firstPresetId();
    QVERIFY(!keyboardPreset.isEmpty());
    m_model->setDeviceGroup(QStringLiteral("controller"));
    QVERIFY(rawExec(QStringLiteral(
        "INSERT OR REPLACE INTO mapping_assignments "
        "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
        "VALUES ('controller', 'game', '%1', '%2', NULL, '2026-09-20', '2026-09-20')")
                        .arg(racerGameKey(), keyboardPreset)));
    m_model->refreshGameTarget();

    QVERIFY(m_model->gameAssignedPresetWrongGroup());
    QCOMPARE(m_model->gameAssignedPresetId(), keyboardPreset);
    QTRY_COMPARE(gameRow->property("assignmentState").toString(),
                 QStringLiteral("wrong_group"));
    // The row says so: its own sentence, its own warning tone - never the normal
    // "uses the preset \"\"" sentence.
    QCOMPARE(gameRow->property("tone").toString(), QStringLiteral("warning"));
    const QString wrongDescription = gameRow->property("description").toString();
    QVERIFY(!wrongDescription.isEmpty());
    QVERIFY(wrongDescription != normalDescription);

    // The picker stays available so the user can choose another preset.
    QObject *gameCombo = combo("presetGameCombo");
    QVERIFY(gameCombo);
    QVERIFY(gameCombo->property("count").toInt() >= 2);
}

QTEST_MAIN(MappingPresetSectionTest)
#include "tst_mappingpresetsection.moc"
