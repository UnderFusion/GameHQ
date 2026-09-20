// MappingPresetModel (cpo-p06, docs/mapping-presets.md section 7).
//
// These tests pin the preset LIBRARY/ASSIGNMENT contract the real Settings
// screen drives, and the boundaries the reviewer set for this leaf:
// - the reserved Built-in artifact is one reusable hidden empty preset, not a
//   name, not user-CRUD-able, and its assignment is created atomically;
// - create + assign is ONE transaction (an invalid target writes nothing);
// - target derivation uses provenance: a durable identity may be persisted as
//   `controller`, a weak one only through a real slot alias, never by relabel;
// - library-only operations (create, rename, an unused duplicate) never touch
//   the runtime seam, while assignment/content/delete call it exactly once;
// - an open draft or capture is never discarded by a preset operation;
// - the first edit from a legacy/built-in route adopts the behavior the target
//   serves today into a user preset without changing any other button meaning,
//   and never deletes the retained legacy rows.
#include "input/MappingPresetModel.h"

#include "input/ActionCatalog.h"
#include "input/ControlId.h"
#include "input/MappingAssignmentResolver.h"
#include "storage/CaptureDatabase.h"

#include <QTemporaryDir>
#include <QTest>

namespace {

MappingPresetRow makeRow(const QString& action, const QString& trigger, int slot = 1,
                         const QString& activation = QStringLiteral("press"), int tapCount = 1,
                         int holdMs = 0, bool unbound = false)
{
    MappingPresetRow row;
    row.actionId = action;
    row.triggerCode = trigger;
    row.slot = slot;
    row.activation = activation;
    row.tapCount = tapCount;
    row.holdMs = holdMs;
    row.unbound = unbound;
    return row;
}

BindingResolver::Binding makeBinding(const QString& group, const QString& profile,
                                     const QString& action, const QString& trigger, int slot = 1)
{
    BindingResolver::Binding binding;
    binding.deviceGroup = group;
    binding.deviceProfile = profile;
    binding.actionId = action;
    binding.slot = slot;
    binding.triggerCode = trigger;
    return binding;
}

// MappingPresetRow has no operator==; a stable signature is what these tests
// actually compare (order-insensitive, every stored field included).
QString rowsSignature(const QVector<MappingPresetRow>& rows)
{
    QStringList parts;
    for (const MappingPresetRow& row : rows) {
        parts.append(QStringLiteral("%1#%2|%3|%4|%5|%6|%7")
                         .arg(row.actionId)
                         .arg(row.slot)
                         .arg(row.triggerCode)
                         .arg(row.activation)
                         .arg(row.holdMs)
                         .arg(row.unbound ? 1 : 0)
                         .arg(row.tapCount));
    }
    parts.sort();
    return parts.join(QLatin1Char(';'));
}

} // namespace

class MappingPresetModelTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void builtinArtifactIsReusedAndReserved();
    void builtinArtifactRefusesNormalCrud();
    void assignmentToBuiltinIsAtomicAndReused();
    void createAssignedWritesNothingOnFailure();
    void provenanceDrivesTheWriteTarget();
    void libraryOperationsNeverTouchTheRuntime();
    void assignmentContentAndDeleteRefreshOnce();
    void pendingEditGuardsDestructiveOperations();
    void adoptAndMaskKeepsLegacyRowsAndAppliesTheEdit();
    void secondEditLandsInTheAdoptedPreset();
    void duplicateForTargetMovesOnlyThatTarget();
    void deleteInUseRefusesThenReassigns();
    void referenceMetadataCountsEveryKind();
    // A preset that is the conversion target of a migration source is recovery
    // evidence, not an ordinary shared preset: storage refuses the reassign, so
    // the model must refuse the delete instead of reporting a partial success.
    void migrationSourceReferenceBlocksDeletion();
    void fallbackAndBuiltinAreDifferentChoices();
    void gameAssignmentWritesTheSessionGameRow();

private:
    int refreshes = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<MappingPresetModel> model;

    // Preset selection is UI state, so the tests drive it through the same API
    // the QML picker uses instead of assuming an implicit selection.
    QString presetIdByName(const QString& name) const
    {
        const QVariantList list = model->presets();
        for (const QVariant& entry : list) {
            const QVariantMap map = entry.toMap();
            if (map.value(QStringLiteral("name")).toString() == name)
                return map.value(QStringLiteral("id")).toString();
        }
        return QString();
    }
    int presetCount() const { return model->presets().size(); }
};

void MappingPresetModelTest::init()
{
    refreshes = 0;
    dir = std::make_unique<QTemporaryDir>();
    QVERIFY(dir->isValid());
    db = std::make_unique<CaptureDatabase>(dir->filePath(QStringLiteral("test.db")));
    QVERIFY(db->open());
    QVERIFY(db->clearAllBindingOverrides());

    model = std::make_unique<MappingPresetModel>(db.get());
    // The production seam set, wired by InputEngine: provenance-derived target,
    // the cpo-c05 pin, and the one runtime refresh.
    model->setRuntimeRefresh([this] { ++refreshes; });
    model->setPinnedProfileProvider([] { return QStringLiteral("controller-abc"); });
    model->setTargetProvider([](const QString& group, const QString& pinned) {
        MappingPresetModel::Provenance provenance;
        provenance.known = !pinned.isEmpty();
        provenance.durable = true;
        provenance.displayName = QStringLiteral("Test pad");
        return MappingPresetModel::targetFromProvenance(group, pinned, provenance);
    });
}

void MappingPresetModelTest::cleanup()
{
    model.reset();
    db.reset();
    dir.reset();
}

void MappingPresetModelTest::builtinArtifactIsReusedAndReserved()
{
    const QString id = db->ensureBuiltinMappingPreset(QStringLiteral("controller"));
    QVERIFY(!id.isEmpty());
    QVERIFY(CaptureDatabase::isBuiltinMappingPresetId(id));
    // Lazy and reusable: a second ensure answers with the same row, and the
    // artifact never shows up as an ordinary library preset.
    QCOMPARE(db->ensureBuiltinMappingPreset(QStringLiteral("controller")), id);
    QCOMPARE(presetCount(), 0);

    const QString keyboard = db->ensureBuiltinMappingPreset(QStringLiteral("keyboard"));
    QVERIFY(CaptureDatabase::isBuiltinMappingPresetId(keyboard));
    QVERIFY(keyboard != id);
    QVERIFY(db->mappingPresetRows(id).isEmpty());
}

void MappingPresetModelTest::builtinArtifactRefusesNormalCrud()
{
    const QString id = db->ensureBuiltinMappingPreset(QStringLiteral("controller"));
    QVERIFY(!id.isEmpty());
    QVERIFY(!db->renameMappingPreset(id, QStringLiteral("Mine")));
    QVERIFY(!db->replaceMappingPresetRows(id, {makeRow(QStringLiteral("global.screenshot"),
                                                       ControlId::FaceSouth)}));
    QVERIFY(!db->deleteMappingPreset(id));
    const QString other =
        db->createMappingPreset(QStringLiteral("controller"), QStringLiteral("Other"));
    QVERIFY(!db->deleteMappingPresetAndReassign(other, id));
    // The reserved display sentinel is not a name a user can create either.
    QVERIFY(db->createMappingPreset(QStringLiteral("controller"), QStringLiteral("\u0001builtin"))
                .isEmpty());
}

void MappingPresetModelTest::assignmentToBuiltinIsAtomicAndReused()
{
    const QString first =
        db->assignMappingTargetToBuiltin(QStringLiteral("controller"),
                                         QStringLiteral("controller"),
                                         QStringLiteral("controller-abc"));
    QVERIFY(CaptureDatabase::isBuiltinMappingPresetId(first));
    QCOMPARE(db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                   QStringLiteral("controller-abc"))
                 .presetId,
             first);
    // A second selection reuses the same artifact instead of creating another.
    const QString second = db->assignMappingTargetToBuiltin(
        QStringLiteral("controller"), QStringLiteral("group_default"), QString());
    QCOMPARE(second, first);
    QCOMPARE(db->listMappingPresets(QStringLiteral("controller")).size(), 1);
    // An invalid target is refused before anything is written.
    QVERIFY(db->assignMappingTargetToBuiltin(QStringLiteral("controller"),
                                             QStringLiteral("game"), QString())
                .isEmpty());
}

void MappingPresetModelTest::createAssignedWritesNothingOnFailure()
{
    const QString preset = db->createMappingPresetAssigned(
        QStringLiteral("controller"), QStringLiteral("Good"),
        {makeRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth)},
        QStringLiteral("controller"), QStringLiteral("controller-abc"));
    QVERIFY(!preset.isEmpty());
    QCOMPARE(db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                   QStringLiteral("controller-abc"))
                 .presetId,
             preset);

    // The same call with an unusable target writes neither metadata nor rows.
    const int before = presetCount();
    QVERIFY(db->createMappingPresetAssigned(QStringLiteral("controller"), QStringLiteral("Ghost"),
                                            {makeRow(QStringLiteral("global.screenshot"),
                                                     ControlId::FaceSouth)},
                                            QStringLiteral("game"), QString())
                .isEmpty());
    QCOMPARE(presetCount(), before);
    QVERIFY(presetIdByName(QStringLiteral("Ghost")).isEmpty());
}

void MappingPresetModelTest::provenanceDrivesTheWriteTarget()
{
    const QString slot = QStringLiteral("xinput.slot0");
    // Durable identity: the logical id itself is the persisted target.
    MappingPresetModel::Provenance durable;
    durable.known = true;
    durable.durable = true;
    durable.displayName = QStringLiteral("DualSense");
    const auto controller = MappingPresetModel::targetFromProvenance(
        QStringLiteral("controller"), QStringLiteral("controller-abc"), durable);
    QCOMPARE(controller.kind, QStringLiteral("controller"));
    QCOMPARE(controller.key, QStringLiteral("controller-abc"));
    QCOMPARE(controller.label, QStringLiteral("DualSense"));

    // Weak/session-local identity: only a real slot alias may be persisted, and
    // the first persistable one in the caller's order wins.
    MappingPresetModel::Provenance weak;
    weak.known = true;
    weak.durable = false;
    weak.slotAliases = {QStringLiteral("winmm.slot1"), QStringLiteral("xinput.slot0")};
    const auto legacy = MappingPresetModel::targetFromProvenance(
        QStringLiteral("controller"), QStringLiteral("controller-weak"), weak);
    QCOMPARE(legacy.kind, QStringLiteral("legacy_slot"));
    QCOMPARE(legacy.key, QStringLiteral("winmm.slot1"));

    MappingPresetModel::Provenance weakSingle;
    weakSingle.known = true;
    weakSingle.durable = false;
    weakSingle.slotAliases = {QStringLiteral("controller-weak"), QStringLiteral("xinput.slot0")};
    const auto legacySingle = MappingPresetModel::targetFromProvenance(
        QStringLiteral("controller"), QStringLiteral("controller-weak"), weakSingle);
    QCOMPARE(legacySingle.kind, QStringLiteral("legacy_slot"));
    QCOMPARE(legacySingle.key, QStringLiteral("xinput.slot0"));

    // A weak identity with no persistable alias has NO target: refused, never
    // guessed by relabeling the logical id.
    MappingPresetModel::Provenance weakOnly;
    weakOnly.known = true;
    weakOnly.durable = false;
    const auto refused = MappingPresetModel::targetFromProvenance(
        QStringLiteral("controller"), QStringLiteral("controller-weak"), weakOnly);
    QVERIFY(refused.kind.isEmpty());
    QCOMPARE(refused.reason, QStringLiteral("weak_identity"));

    // Unpinned (shared) editing and the single-route groups use the group default.
    const auto shared = MappingPresetModel::targetFromProvenance(
        QStringLiteral("controller"), QString(), durable);
    QCOMPARE(shared.kind, QStringLiteral("group_default"));
    const auto keyboard = MappingPresetModel::targetFromProvenance(
        QStringLiteral("keyboard"), QString(), durable);
    QCOMPARE(keyboard.kind, QStringLiteral("group_default"));
}

void MappingPresetModelTest::libraryOperationsNeverTouchTheRuntime()
{
    QVERIFY(model->createPreset(QStringLiteral("First")));
    const QString id = presetIdByName(QStringLiteral("First"));
    QVERIFY(!id.isEmpty());
    QCOMPARE(refreshes, 0);

    QVERIFY(model->renameSelected(QStringLiteral("Renamed")));
    // The id is the identity: rename keeps it, and every assignment keeps
    // working without the runtime being disturbed.
    QCOMPARE(model->selectedPresetId(), id);
    QCOMPARE(db->mappingPreset(id).name, QStringLiteral("Renamed"));
    QVERIFY(model->duplicateSelected(QStringLiteral("Copy")));
    QCOMPARE(presetCount(), 2);
    QCOMPARE(refreshes, 0);
}

void MappingPresetModelTest::assignmentContentAndDeleteRefreshOnce()
{
    QVERIFY(model->createPreset(QStringLiteral("Mine")));
    const QString id = presetIdByName(QStringLiteral("Mine"));
    QVERIFY(model->applyAssignment(id));
    QCOMPARE(refreshes, 1);
    QCOMPARE(model->assignedPresetId(), id);
    QVERIFY(!model->assignedToFallback());

    // Content edit through the editor sink: one batch, one refresh.
    MappingPresetModel::ContentEdit edit;
    edit.actionId = QStringLiteral("global.screenshot");
    edit.slot = 1;
    edit.row = makeRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth);
    QVERIFY(model->applyContentEdits({edit}));
    QCOMPARE(refreshes, 2);
    QCOMPARE(db->mappingPresetRows(id).size(), 1);

    // A second, unused preset can be deleted without touching the runtime...
    QVERIFY(model->createPreset(QStringLiteral("Spare")));
    const QString spare = presetIdByName(QStringLiteral("Spare"));
    QVERIFY(model->selectPreset(spare));
    QVERIFY(model->deleteSelected());
    QCOMPARE(refreshes, 2);
    QVERIFY(db->mappingPreset(spare).id.isEmpty());
    // ...while the assignment itself is untouched.
    QCOMPARE(model->assignedPresetId(), id);
}

void MappingPresetModelTest::pendingEditGuardsDestructiveOperations()
{
    QVERIFY(model->createPreset(QStringLiteral("Mine")));
    const QString mine = presetIdByName(QStringLiteral("Mine"));
    QVERIFY(model->createPreset(QStringLiteral("Other")));
    const QString other = presetIdByName(QStringLiteral("Other"));
    QVERIFY(model->selectPreset(mine));

    bool pending = true;
    model->setPendingEditProvider([&pending] { return pending; });
    // Selection, assignment, delete and duplicate-for-this-controller would all
    // change what an open draft refers to: refused, not applied.
    QVERIFY(!model->selectPreset(other));
    QVERIFY(!model->applyAssignment(other));
    QVERIFY(!model->duplicateSelectedForTarget(QStringLiteral("Copy")));
    QVERIFY(!model->deleteSelected());
    // Creating a preset disturbs nothing, and rename stays allowed: it is
    // display-only and cannot orphan a draft.
    QVERIFY(model->createPreset(QStringLiteral("Third")));
    QVERIFY(model->renameSelected(QStringLiteral("Renamed while editing")));
    QCOMPARE(db->mappingPreset(mine).name, QStringLiteral("Renamed while editing"));

    pending = false;
    QVERIFY(model->selectPreset(other));
    QVERIFY(model->applyAssignment(other));
}

void MappingPresetModelTest::adoptAndMaskKeepsLegacyRowsAndAppliesTheEdit()
{
    // Legacy rows exist and serve the route today.
    QVERIFY(db->upsertBindingOverride({QStringLiteral("controller"),
                                       QStringLiteral("controller-abc"),
                                       QStringLiteral("global.screenshot"), 1,
                                       ControlId::FaceSouth, QStringLiteral("press"), 0, false,
                                       1}));
    const int legacyBefore = db->listBindingOverrides().size();
    model->setEffectiveTableProvider([](const QString&, const QString&) {
        QVector<BindingResolver::Binding> table;
        table.append(makeBinding(QStringLiteral("controller"), QStringLiteral("controller-abc"),
                                 QStringLiteral("global.screenshot"), ControlId::FaceSouth));
        table.append(makeBinding(QStringLiteral("controller"), QStringLiteral("controller-abc"),
                                 QStringLiteral("global.save_replay"), ControlId::FaceEast));
        return table;
    });

    MappingPresetModel::ContentEdit edit;
    edit.actionId = QStringLiteral("global.save_replay");
    edit.slot = 1;
    edit.row = makeRow(QStringLiteral("global.save_replay"), ControlId::FaceWest);
    QVERIFY(model->applyContentEdits({edit}));
    QCOMPARE(refreshes, 1);

    // The first edit adopted the behavior the target served TODAY and changed
    // only the requested row: every other button keeps its meaning.
    const QString adopted =
        db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                              QStringLiteral("controller-abc"))
            .presetId;
    QVERIFY(!adopted.isEmpty());
    const QVector<MappingPresetRow> rows = db->mappingPresetRows(adopted);
    QCOMPARE(rows.size(), 2);
    for (const MappingPresetRow& row : rows) {
        if (row.actionId == QStringLiteral("global.screenshot"))
            QCOMPARE(row.triggerCode, QString(ControlId::FaceSouth));
        else if (row.actionId == QStringLiteral("global.save_replay"))
            QCOMPARE(row.triggerCode, QString(ControlId::FaceWest));
        else
            QFAIL("unexpected action in the adopted preset");
    }
    // Retained legacy rows are recovery evidence: never deleted by adoption.
    QCOMPARE(db->listBindingOverrides().size(), legacyBefore);
}

void MappingPresetModelTest::secondEditLandsInTheAdoptedPreset()
{
    model->setEffectiveTableProvider([](const QString&, const QString&) {
        QVector<BindingResolver::Binding> table;
        table.append(makeBinding(QStringLiteral("controller"), QStringLiteral("controller-abc"),
                                 QStringLiteral("global.screenshot"), ControlId::FaceSouth));
        return table;
    });
    MappingPresetModel::ContentEdit edit;
    edit.actionId = QStringLiteral("global.screenshot");
    edit.slot = 1;
    edit.row = makeRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth);
    QVERIFY(model->applyContentEdits({edit}));
    const QString adopted = model->assignedPresetId();
    QVERIFY(!adopted.isEmpty());
    const int presetsAfterAdopt = presetCount();

    // The second edit is a plain content change: no new preset, same winner.
    MappingPresetModel::ContentEdit second;
    second.actionId = QStringLiteral("global.save_replay");
    second.slot = 1;
    second.row = makeRow(QStringLiteral("global.save_replay"), ControlId::FaceEast);
    QVERIFY(model->applyContentEdits({second}));
    QCOMPARE(presetCount(), presetsAfterAdopt);
    QCOMPARE(model->assignedPresetId(), adopted);
    QCOMPARE(db->mappingPresetRows(adopted).size(), 2);
    QCOMPARE(refreshes, 2);
}

// The game row (cpo-p07): the same three choices as the device picker, scoped to
// the session game, written through the same typed-assignment API and the same
// single runtime seam. No second source of truth for "which game am I in".
void MappingPresetModelTest::gameAssignmentWritesTheSessionGameRow()
{
    // No game in session: refused, and nothing is written.
    model->setGameTargetProvider([] { return MappingPresetModel::GameTarget(); });
    model->refreshGameTarget();
    QVERIFY(!model->gameAvailable());
    QVERIFY(!model->applyGameAssignment(QStringLiteral("preset-anything")));
    QCOMPARE(model->noticeKind(), QStringLiteral("game_unavailable"));
    QCOMPARE(refreshes, 0);
    model->clearNotice();

    const QString gameKey = MappingAssignmentResolver::canonicalGameKey(
        QStringLiteral("C:\\Games\\Racer\\Racer.exe"));
    QVERIFY(!gameKey.isEmpty());
    MappingPresetModel::GameTarget target;
    target.key = gameKey;
    target.label = QStringLiteral("Racer");
    target.rowId = 7;
    model->setGameTargetProvider([target] { return target; });
    model->refreshGameTarget();
    QVERIFY(model->gameAvailable());
    QCOMPARE(model->gameLabel(), QStringLiteral("Racer"));
    QVERIFY(model->gameAssignedToFallback());

    const QString preset = db->createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Racer preset"),
        {makeRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth)});
    QVERIFY(!preset.isEmpty());

    QVERIFY(model->applyGameAssignment(preset));
    QCOMPARE(refreshes, 1);
    QCOMPARE(model->gameAssignedPresetId(), preset);
    QCOMPARE(model->gameAssignedPresetName(), QStringLiteral("Racer preset"));
    const MappingAssignment row =
        db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("game"), gameKey);
    QCOMPARE(row.presetId, preset);
    QCOMPARE(row.targetKey, gameKey);
    QCOMPARE(row.gameRowId, 7);

    // Built-in defaults for this game: the explicit empty winner, atomically.
    QVERIFY(model->applyGameAssignment(MappingPresetModel::builtinChoiceToken()));
    QCOMPARE(refreshes, 2);
    QVERIFY(model->gameAssignedToBuiltin());
    QVERIFY(CaptureDatabase::isBuiltinMappingPresetId(
        db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("game"), gameKey)
            .presetId));

    // Follow the chain below the game.
    QVERIFY(model->applyGameAssignment(QString()));
    QCOMPARE(refreshes, 3);
    QVERIFY(model->gameAssignedToFallback());
    QCOMPARE(db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("game"), gameKey)
                 .presetId,
             QString());

    // An unknown preset is refused and changes nothing.
    QVERIFY(!model->applyGameAssignment(QStringLiteral("preset-missing")));
    QCOMPARE(model->noticeKind(), QStringLiteral("unknown_preset"));
    QCOMPARE(refreshes, 3);
    model->clearNotice();

    // An open draft blocks the write, like every other assignment.
    model->setPendingEditProvider([] { return true; });
    QVERIFY(!model->applyGameAssignment(preset));
    QCOMPARE(model->noticeKind(), QStringLiteral("pending_edit"));
    QCOMPARE(refreshes, 3);

    // Re-publishing the session state is never a switch: a foreground poll that
    // resolves to the same game must not invalidate a gesture.
    model->refreshGameTarget();
    QCOMPARE(refreshes, 3);
}

void MappingPresetModelTest::duplicateForTargetMovesOnlyThatTarget()
{
    // A shared preset: the pinned controller AND the group default use it.
    const QString shared = db->createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Shared"),
        {makeRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth)});
    QVERIFY(!shared.isEmpty());
    QVERIFY(db->setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                     QStringLiteral("controller-abc"), shared));
    QVERIFY(db->setMappingAssignment(QStringLiteral("controller"), QStringLiteral("group_default"),
                                     QString(), shared));
    model->refresh();
    QVERIFY(model->selectPreset(shared));
    QVERIFY(model->selectedShared());
    QCOMPARE(model->controllerUses(), 1);
    QCOMPARE(model->groupDefaultUses(), 1);

    // Duplicate + assign is one transaction, and only the pinned target moved.
    QVERIFY(model->duplicateSelectedForTarget(QStringLiteral("Shared (this pad)")));
    const QString copy = presetIdByName(QStringLiteral("Shared (this pad)"));
    QVERIFY(!copy.isEmpty());
    QVERIFY(copy != shared);
    QCOMPARE(rowsSignature(db->mappingPresetRows(copy)),
             rowsSignature(db->mappingPresetRows(shared)));
    QCOMPARE(db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                   QStringLiteral("controller-abc"))
                 .presetId,
             copy);
    QCOMPARE(db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("group_default"),
                                   QString())
                 .presetId,
             shared);
    QCOMPARE(refreshes, 1);
}

void MappingPresetModelTest::deleteInUseRefusesThenReassigns()
{
    QVERIFY(model->createPreset(QStringLiteral("Mine")));
    const QString mine = presetIdByName(QStringLiteral("Mine"));
    QVERIFY(model->createPreset(QStringLiteral("Other")));
    const QString other = presetIdByName(QStringLiteral("Other"));
    QVERIFY(model->selectPreset(mine));
    QVERIFY(model->applyAssignment(mine));

    // Referenced: refused with a copy the UI can act on, and nothing changed.
    QVERIFY(!model->deleteSelected());
    QCOMPARE(model->noticeKind(), QStringLiteral("delete_in_use"));
    QVERIFY(!db->mappingPreset(mine).id.isEmpty());

    // Explicit reassign resolves the reference and the delete in one go.
    QVERIFY(model->deleteSelected(other));
    QVERIFY(db->mappingPreset(mine).id.isEmpty());
    QCOMPARE(db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                   QStringLiteral("controller-abc"))
                 .presetId,
             other);
    QCOMPARE(refreshes, 2);
}

void MappingPresetModelTest::referenceMetadataCountsEveryKind()
{
    QVERIFY(model->createPreset(QStringLiteral("Shared")));
    const QString shared = presetIdByName(QStringLiteral("Shared"));
    QVERIFY(db->setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                     QStringLiteral("controller-abc"), shared));
    QVERIFY(db->setMappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), shared));
    QVERIFY(db->setMappingAssignment(QStringLiteral("controller"), QStringLiteral("group_default"),
                                     QString(), shared));
    QVERIFY(db->setMappingAssignment(QStringLiteral("controller"), QStringLiteral("game"),
                                     QStringLiteral("C:/Games/Test.exe"), shared));
    QVERIFY(model->selectPreset(shared));
    model->refresh();
    QCOMPARE(model->controllerUses(), 2);
    QCOMPARE(model->groupDefaultUses(), 1);
    QCOMPARE(model->gameUses(), 1);
    QVERIFY(model->selectedShared());
}

void MappingPresetModelTest::migrationSourceReferenceBlocksDeletion()
{
    const QString converted = db->createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Migrated"),
        {}, QStringLiteral("migration"));
    const QString other = db->createMappingPreset(QStringLiteral("controller"),
                                                  QStringLiteral("Other"));
    QVERIFY(!converted.isEmpty() && !other.isEmpty());

    MappingPresetSource source;
    source.deviceGroup       = QStringLiteral("controller");
    source.sourceKey         = QStringLiteral("controller-0123456789abcdef");
    source.convertedPresetId = converted;
    source.note              = QStringLiteral("converted, durability unproven");
    QVERIFY(db->upsertMappingPresetSource(source));

    model->refresh();
    QVERIFY(model->selectPreset(converted));
    QCOMPARE(model->migrationUses(), 1);
    // Sharing is surfaced, but not as an ordinary movable set: the assignment
    // count stays zero, which is exactly what the delete dialog keys on.
    QVERIFY(model->selectedShared());
    QCOMPARE(model->controllerUses() + model->groupDefaultUses() + model->gameUses(), 0);

    // No reassign target: refused because the preset is referenced at all.
    QVERIFY(!model->deleteSelected());
    QCOMPARE(model->noticeKind(), QStringLiteral("delete_in_use"));

    // With a valid reassign target storage still refuses: a migration source is
    // recovery evidence for one historical key and cannot follow another preset.
    QVERIFY(!model->deleteSelected(other));
    QCOMPARE(model->noticeKind(), QStringLiteral("delete_failed"));

    QVERIFY(!db->mappingPreset(converted).id.isEmpty());
    QVERIFY(!db->mappingPreset(other).id.isEmpty());
    QCOMPARE(db->mappingPresetSource(QStringLiteral("controller"), source.sourceKey)
                 .convertedPresetId,
             converted);
}

void MappingPresetModelTest::fallbackAndBuiltinAreDifferentChoices()
{
    QVERIFY(model->createPreset(QStringLiteral("Mine")));
    const QString mine = presetIdByName(QStringLiteral("Mine"));

    // "Remove assignment / follow fallback": no row at all.
    QVERIFY(model->applyAssignment(QString()));
    QVERIFY(model->assignedToFallback());
    QVERIFY(!model->assignedToBuiltin());

    // "Built-in defaults": an explicit empty winner that masks every layer
    // below it, created lazily and reused by the next selection.
    QVERIFY(model->applyAssignment(MappingPresetModel::builtinChoiceToken()));
    QVERIFY(model->assignedToBuiltin());
    QVERIFY(!model->assignedToFallback());
    const QString reserved = db->mappingAssignment(QStringLiteral("controller"),
                                                   QStringLiteral("controller"),
                                                   QStringLiteral("controller-abc"))
                                 .presetId;
    QVERIFY(CaptureDatabase::isBuiltinMappingPresetId(reserved));
    QVERIFY(db->mappingPresetRows(reserved).isEmpty());
    // The library still shows only the user's preset.
    QCOMPARE(presetCount(), 1);

    QVERIFY(model->applyAssignment(mine));
    QVERIFY(!model->assignedToBuiltin());
    QCOMPARE(model->assignedPresetId(), mine);
    QVERIFY(model->applyAssignment(MappingPresetModel::builtinChoiceToken()));
    QCOMPARE(db->mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                   QStringLiteral("controller-abc"))
                 .presetId,
             reserved);
}

QTEST_GUILESS_MAIN(MappingPresetModelTest)
#include "tst_mappingpresetmodel.moc"
