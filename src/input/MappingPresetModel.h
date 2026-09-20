#pragma once

#include "input/BindingResolver.h"
#include "storage/CaptureDatabase.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class CaptureDatabase;

// QML-facing preset library + assignment facade (cpo-p06,
// docs/mapping-presets.md section 7).
//
// Responsibilities
// - one device group's user preset library: list, select, create, duplicate,
//   rename, delete / delete-and-reassign;
// - the typed assignment of the editing target: pinned controller, its
//   persistable legacy slot alias, or the group default;
// - the virtual "Built-in defaults" choice, backed by ONE reusable reserved
//   empty preset per group (never a new one per click, never a user-visible
//   magic name);
// - reference metadata, so a shared preset says who else it serves and the
//   copy-on-write path ("duplicate for this controller") is explicit;
// - the unsaved-edit guard: nothing here discards an open draft or a capture.
//
// Boundaries
// - Decision-free: it never resolves a winner (MappingAssignmentResolver,
//   cpo-p04) and never builds a table (InputEngine::refreshResolvedPreset,
//   cpo-p05). It writes storage, then asks the engine to re-resolve.
// - The runtime seam is called ONLY for operations that can change an
//   effective table. create / duplicate-unused / rename are library-only:
//   they must never invalidate an in-flight gesture (reviewer correction 4).
// - Target derivation uses provenance, never key syntax: the engine supplies
//   the typed target (durable identity -> `controller`, a persistable slot
//   alias -> `legacy_slot`), and a weak or session-local identity is refused
//   instead of being persisted under a prettier label.
class MappingPresetModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString deviceGroup READ deviceGroup WRITE setDeviceGroup NOTIFY deviceGroupChanged)
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    Q_PROPERTY(QString selectedPresetId READ selectedPresetId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedPresetName READ selectedPresetName NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedIsBuiltin READ selectedIsBuiltin NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedEditable READ selectedEditable NOTIFY selectionChanged)
    Q_PROPERTY(QString targetKind READ targetKind NOTIFY targetChanged)
    Q_PROPERTY(QString targetKey READ targetKey NOTIFY targetChanged)
    Q_PROPERTY(QString targetLabel READ targetLabel NOTIFY targetChanged)
    Q_PROPERTY(bool targetAvailable READ targetAvailable NOTIFY targetChanged)
    Q_PROPERTY(QString targetUnavailableReason READ targetUnavailableReason NOTIFY targetChanged)
    Q_PROPERTY(QString assignedPresetId READ assignedPresetId NOTIFY assignmentChanged)
    Q_PROPERTY(QString assignedPresetName READ assignedPresetName NOTIFY assignmentChanged)
    Q_PROPERTY(bool assignedToFallback READ assignedToFallback NOTIFY assignmentChanged)
    Q_PROPERTY(bool assignedToBuiltin READ assignedToBuiltin NOTIFY assignmentChanged)
    Q_PROPERTY(bool assignedPresetMissing READ assignedPresetMissing NOTIFY assignmentChanged)
    Q_PROPERTY(bool assignedPresetWrongGroup READ assignedPresetWrongGroup NOTIFY assignmentChanged)
    Q_PROPERTY(int controllerUses READ controllerUses NOTIFY usesChanged)
    Q_PROPERTY(int groupDefaultUses READ groupDefaultUses NOTIFY usesChanged)
    Q_PROPERTY(int gameUses READ gameUses NOTIFY usesChanged)
    Q_PROPERTY(int migrationUses READ migrationUses NOTIFY usesChanged)
    Q_PROPERTY(bool selectedShared READ selectedShared NOTIFY usesChanged)
    Q_PROPERTY(bool pendingEdit READ pendingEdit NOTIFY pendingEditChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(QString noticeKind READ noticeKind NOTIFY noticeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    // The typed editing target. `kind` empty means "no persistable target":
    // controller-specific assignment is then refused rather than guessed.
    struct Target
    {
        QString kind;    // "controller" | "legacy_slot" | "group_default" | ""
        QString key;
        QString profile; // route profile to snapshot for adoption; empty for the group default
        QString label;   // display name for generated preset names and UI copy
        QString reason;  // why it is unavailable ("" when available)
    };
    using TargetProvider = std::function<Target(const QString& deviceGroup,
                                                const QString& pinnedProfile)>;
    // The inputs the provenance rule needs, so the rule itself is testable
    // without a live provider registry.
    struct Provenance
    {
        bool known = false;        // the registry knows this logical profile
        bool durable = false;      // Strong identity confidence
        QString displayName;
        QStringList slotAliases;   // persistable compatibility keys, in order
    };
    // The ONE write-target rule (cpo-p04/p05 provenance reused): a proven
    // durable identity may be persisted as `controller`; a weak or session-local
    // logical id may only be persisted through a real persistable slot alias
    // (`xinput.slotN` / `winmm.slotN`); otherwise there is no persistable target
    // and controller-specific assignment is refused. Key text is never
    // classified here - only the caller's declared provenance is.
    static Target targetFromProvenance(const QString& deviceGroup, const QString& pinnedProfile,
                                       const Provenance& provenance);
    using PinProvider = std::function<QString()>;
    // One requested (action, slot) change; the unit the editor hands over. The
    // preset decision (which preset owns the target, or adoption) stays here.
    struct ContentEdit
    {
        QString actionId;
        int slot = 1;
        MappingPresetRow row;   // ignored when `remove`
        bool remove = false;
    };
    using ContentEditSink = std::function<bool(const QVector<ContentEdit>& edits)>;

    explicit MappingPresetModel(CaptureDatabase* database, QObject* parent = nullptr);

    // ------------------------------------------------------ engine seams ---
    // InputEngine::refreshResolvedPreset(): called only for mutations that can
    // change an effective table.
    void setRuntimeRefresh(std::function<void()> refresh);
    // The table the target serves today, used only by adopt-and-mask.
    void setEffectiveTableProvider(
        std::function<QVector<BindingResolver::Binding>(const QString& group,
                                                        const QString& profile)> provider);
    void setTargetProvider(TargetProvider provider);
    // The cpo-c05 pin: the editor's explicitly selected controller profile, or
    // empty while editing is shared (group-wide).
    void setPinnedProfileProvider(PinProvider provider);
    // True while the binding editor holds an unsaved draft or an active
    // capture. Guarded operations are refused, never applied destructively.
    void setPendingEditProvider(std::function<bool()> provider);
    ContentEditSink contentEditSink();
    // One batch = one storage transaction: the editor's whole request lands in
    // exactly one preset, in one adopted preset, or nowhere.
    bool applyContentEdits(const QVector<ContentEdit>& edits);

    QString deviceGroup() const { return m_deviceGroup; }
    void setDeviceGroup(const QString& group);

    QVariantList presets() const { return m_presetList; }
    QString selectedPresetId() const { return m_selectedPresetId; }
    QString selectedPresetName() const;
    bool selectedIsBuiltin() const;
    bool selectedEditable() const;
    Target target() const;
    QString targetKind() const { return target().kind; }
    QString targetKey() const { return target().key; }
    QString targetLabel() const { return target().label; }
    bool targetAvailable() const { return !target().kind.isEmpty(); }
    QString targetUnavailableReason() const { return target().reason; }
    QString assignedPresetId() const;
    QString assignedPresetName() const;
    bool assignedToFallback() const;
    bool assignedToBuiltin() const;
    bool assignedPresetMissing() const;
    bool assignedPresetWrongGroup() const;
    int controllerUses() const { return m_uses.controllerUses; }
    int groupDefaultUses() const { return m_uses.groupDefaultUses; }
    int gameUses() const { return m_uses.gameUses; }
    int migrationUses() const { return m_uses.migrationUses; }
    bool selectedShared() const;
    bool pendingEdit() const { return m_pendingEditProvider ? m_pendingEditProvider() : false; }
    QString notice() const { return m_notice; }
    QString noticeKind() const { return m_noticeKind; }
    bool busy() const { return m_busy; }

    // The sentinel QML uses for "Built-in defaults" in applyAssignment(); the
    // reserved preset id never leaks into presentation.
    static QString builtinChoiceToken() { return QStringLiteral("@builtin"); }
    static QString fallbackChoiceToken() { return QString(); }

    Q_INVOKABLE void refresh();
    // Selection is UI state (the preset being edited); it never changes an
    // assignment on its own. Refused while an unsaved draft is open.
    Q_INVOKABLE bool selectPreset(const QString& presetId);
    Q_INVOKABLE bool createPreset(const QString& name);
    Q_INVOKABLE bool duplicateSelected(const QString& name);
    Q_INVOKABLE bool renameSelected(const QString& name);
    // `reassignToId` empty: refuse while referenced; otherwise the whole delete
    // and the reassignment are one storage transaction.
    Q_INVOKABLE bool deleteSelected(const QString& reassignToId = QString());
    // "" = remove the assignment (follow fallback); "@builtin" = explicit empty
    // winner; anything else = assign that preset.
    Q_INVOKABLE bool applyAssignment(const QString& presetId);
    // Atomic duplicate + assign for the current target ("duplicate for this
    // controller") - the explicit copy-on-write path for a shared preset.
    Q_INVOKABLE bool duplicateSelectedForTarget(const QString& name);
    Q_INVOKABLE void clearNotice();

signals:
    void deviceGroupChanged();
    void presetsChanged();
    void selectionChanged();
    void targetChanged();
    void assignmentChanged();
    void usesChanged();
    void pendingEditChanged();
    void noticeChanged();
    void busyChanged();

private:
    struct Uses
    {
        int controllerUses = 0;
        int groupDefaultUses = 0;
        int gameUses = 0;
        int migrationUses = 0;
    };

    QString pinnedProfile() const { return m_pinProvider ? m_pinProvider() : QString(); }
    void rebuildPresets();
    void rebuildAssignment();
    void rebuildUses();
    void notifyRuntimeChange();
    bool guarded();
    bool setNotice(const QString& kind, const QString& text);
    // The write path behind both the QML "content edit" and the editor sink.
    bool applyContentEdit(const QString& actionId, int slot, const MappingPresetRow& row,
                          bool remove);
    static bool applyEditToRows(const QString& deviceGroup, QVector<MappingPresetRow>& rows,
                                const ContentEdit& edit);
    static bool applyRowToRows(const QString& deviceGroup, QVector<MappingPresetRow>& rows,
                               const QString& actionId, int slot, const MappingPresetRow& row,
                               bool remove);
    static QVector<MappingPresetRow> rowsFromTable(const QString& deviceGroup,
                                                   const QVector<BindingResolver::Binding>& table);
    QString uniquePresetName(const QString& base) const;
    QString selectedOrAssignedPresetId() const;

    CaptureDatabase* m_database = nullptr;
    std::function<void()> m_refreshRuntime;
    std::function<QVector<BindingResolver::Binding>(const QString&, const QString&)>
        m_effectiveTable;
    TargetProvider m_targetProvider;
    PinProvider m_pinProvider;
    std::function<bool()> m_pendingEditProvider;

    QString m_deviceGroup = QStringLiteral("controller");
    QVariantList m_presetList;
    QString m_selectedPresetId;
    QString m_assignedPresetId;
    QString m_assignedPresetName;
    bool m_assignedMissing = false;
    bool m_assignedWrongGroup = false;
    Uses m_uses;
    QString m_notice;
    QString m_noticeKind;
    bool m_busy = false;
};
