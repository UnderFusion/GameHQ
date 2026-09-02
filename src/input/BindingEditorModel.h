#pragma once

#include "input/BindingRelation.h"
#include "input/BindingResolver.h"
#include "input/ControlId.h"
#include "storage/CaptureDatabase.h"

#include <QObject>
#include <QSet>
#include <QVariantList>
#include <functional>

class BindingRuntime;
class CaptureDatabase;

// QML-facing editor facade over the canonical binding resolver/runtime.
class BindingEditorModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString deviceGroup READ deviceGroup WRITE setDeviceGroup NOTIFY deviceGroupChanged)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(bool controllerSpecific READ controllerSpecific WRITE setControllerSpecific NOTIFY controllerSpecificChanged)
    Q_PROPERTY(bool controllerSpecificAvailable READ controllerSpecificAvailable NOTIFY controllerProfileChanged)
    Q_PROPERTY(QString controllerName READ controllerName NOTIFY controllerProfileChanged)
    Q_PROPERTY(bool captureActive READ captureActive NOTIFY captureChanged)
    Q_PROPERTY(QString capturePrompt READ capturePrompt NOTIFY captureChanged)
    Q_PROPERTY(bool conflictPending READ conflictPending NOTIFY conflictChanged)
    Q_PROPERTY(QString conflictMessage READ conflictMessage NOTIFY conflictChanged)
    Q_PROPERTY(bool compatibilityPending READ compatibilityPending NOTIFY compatibilityChanged)
    Q_PROPERTY(QString compatibilityMessage READ compatibilityMessage NOTIFY compatibilityChanged)
    // Binding notices live on their own channel. They must never be routed
    // through input.controllerWarning, which is reserved for HidHide/cloaked-pad
    // state — a key conflict overwriting a hidden-controller warning would hide
    // the one message the user cannot recover from on their own.
    Q_PROPERTY(QString relationNotice READ relationNotice NOTIFY relationNoticeChanged)
    Q_PROPERTY(QString relationKind READ relationKind NOTIFY relationNoticeChanged)
    Q_PROPERTY(QString validationError READ validationError NOTIFY relationNoticeChanged)
    Q_PROPERTY(QString lastFiredAction READ lastFiredAction NOTIFY lastFiredActionChanged)
    // True when the selected controller has a stable hardware identity and
    // pre-identity "any pad in this slot" override rows exist that the user
    // may want to promote to it. Promotion is always this explicit action —
    // slot rows are never silently rewritten or broadened.
    Q_PROPERTY(bool legacyCopyAvailable READ legacyCopyAvailable NOTIFY rowsChanged)
    // --- Edit Assignment dialog -------------------------------------------
    // A draft, not a live binding: nothing reaches the database until save().
    // The dialog is the only place a gesture or a combination can be chosen,
    // which is why the draft carries the whole pattern rather than just a
    // captured trigger.
    Q_PROPERTY(bool editorOpen READ editorOpen NOTIFY editorChanged)
    Q_PROPERTY(QString editorActionLabel READ editorActionLabel NOTIFY editorChanged)
    Q_PROPERTY(QString editorScopeLabel READ editorScopeLabel NOTIFY editorChanged)
    Q_PROPERTY(int editorSlot READ editorSlot NOTIFY editorChanged)
    Q_PROPERTY(QString editorTriggerKind READ editorTriggerKind NOTIFY editorChanged)
    Q_PROPERTY(QString editorTriggerLabel READ editorTriggerLabel NOTIFY editorChanged)
    Q_PROPERTY(QString editorFirstControlLabel READ editorFirstControlLabel NOTIFY editorChanged)
    Q_PROPERTY(QString editorSecondControlLabel READ editorSecondControlLabel NOTIFY editorChanged)
    Q_PROPERTY(QString editorTriggerHint READ editorTriggerHint NOTIFY editorChanged)
    // "idle" | "first" | "second" — capture mode is explicit, so controller
    // navigation keeps working until the user asks to record a button.
    Q_PROPERTY(QString editorCaptureStep READ editorCaptureStep NOTIFY editorChanged)
    Q_PROPERTY(QString editorGestureKind READ editorGestureKind NOTIFY editorChanged)
    Q_PROPERTY(int editorTapCount READ editorTapCount NOTIFY editorChanged)
    Q_PROPERTY(int editorHoldMs READ editorHoldMs NOTIFY editorChanged)
    Q_PROPERTY(bool editorGestureLocked READ editorGestureLocked NOTIFY editorChanged)
    Q_PROPERTY(bool editorCombinationAvailable READ editorCombinationAvailable NOTIFY editorChanged)
    Q_PROPERTY(bool editorCanSave READ editorCanSave NOTIFY editorChanged)
    Q_PROPERTY(QString editorNotice READ editorNotice NOTIFY editorChanged)
    Q_PROPERTY(QString editorNoticeKind READ editorNoticeKind NOTIFY editorChanged)
public:
    // Registers `chord` for actionId/slot with the OS, or releases the slot when
    // `chord` is empty. Returns false and fills `reason` when Windows refuses
    // the chord. Injected by InputEngine; left null in tests that do not care
    // about global hotkeys, and never called for non-keyboard device groups.
    using HotkeyApply = std::function<bool(const QString& actionId, int slot,
                                           const QString& chord, QString* reason)>;
    // Persists one override row. Swappable so a test can force the DB half of
    // the transaction to fail without corrupting a real database.
    using PersistRow = std::function<bool(const BindingOverrideRow& row)>;

    BindingEditorModel(CaptureDatabase* database, BindingRuntime* runtime,
                       std::function<void()> reloadRuntime, QObject* parent = nullptr);

    void setHotkeyApply(HotkeyApply apply);
    void setPersistRow(PersistRow persist);

    QString deviceGroup() const { return m_deviceGroup; }
    void setDeviceGroup(const QString& group);
    QVariantList rows() const { return m_rows; }
    bool controllerSpecific() const { return m_controllerSpecific; }
    void setControllerSpecific(bool specific);
    bool controllerSpecificAvailable() const { return !m_controllerFingerprint.isEmpty(); }
    QString controllerName() const { return m_controllerName; }
    bool captureActive() const { return m_captureActive; }
    QString capturePrompt() const { return m_capturePrompt; }
    bool conflictPending() const { return m_conflictPending; }
    QString conflictMessage() const { return m_conflictMessage; }
    bool compatibilityPending() const { return m_compatibilityPending; }
    QString compatibilityMessage() const { return m_compatibilityMessage; }
    QString relationNotice() const { return m_relationNotice; }
    QString relationKind() const { return m_relationKind; }
    QString validationError() const { return m_validationError; }
    QString lastFiredAction() const { return m_lastFiredAction; }

    Q_INVOKABLE void beginCapture(const QString& actionId, int slot);
    Q_INVOKABLE void cancelCapture();
    Q_INVOKABLE void clearBinding(const QString& actionId, int slot);
    Q_INVOKABLE void resetBinding(const QString& actionId, int slot);
    Q_INVOKABLE void resetAction(const QString& actionId);
    Q_INVOKABLE void resetCurrentProfile();
    Q_INVOKABLE void resetAllBindings();
    Q_INVOKABLE void confirmConflict();
    Q_INVOKABLE void dismissConflict();
    Q_INVOKABLE void dismissRelationNotice();
    // Re-opens capture for the same action/slot so the user can pick another
    // trigger without hunting for the row again ("Choose another" in the dialog).
    Q_INVOKABLE void retryConflictCapture();
    Q_INVOKABLE void confirmCompatibility();
    Q_INVOKABLE void dismissCompatibility();
    Q_INVOKABLE void retryCompatibilityCapture();
    bool legacyCopyAvailable() const;
    Q_INVOKABLE void copyLegacyOverridesToController();

    bool editorOpen() const { return m_editorOpen; }
    QString editorActionLabel() const { return m_editorActionLabel; }
    QString editorScopeLabel() const { return m_editorScopeLabel; }
    int editorSlot() const { return m_editorSlot; }
    QString editorTriggerKind() const { return m_editorTriggerKind; }
    QString editorTriggerLabel() const;
    QString editorFirstControlLabel() const { return controlLabel(m_editorFirstControl); }
    QString editorSecondControlLabel() const { return controlLabel(m_editorSecondControl); }
    QString editorTriggerHint() const;
    QString editorCaptureStep() const { return m_editorCaptureStep; }
    QString editorGestureKind() const { return m_editorGesture.activationCode(); }
    int editorTapCount() const { return m_editorGesture.tapCount; }
    int editorHoldMs() const { return m_editorGesture.holdMs; }
    // Combinations are press-only in v1, so the gesture picker is fixed there.
    bool editorGestureLocked() const { return m_editorTriggerKind == QLatin1String("combination"); }
    bool editorCombinationAvailable() const { return m_deviceGroup == QLatin1String("controller"); }
    bool editorCanSave() const;
    QString editorNotice() const { return m_editorNotice; }
    QString editorNoticeKind() const { return m_editorNoticeKind; }

    Q_INVOKABLE void openAssignmentEditor(const QString& actionId, int slot);
    Q_INVOKABLE void closeAssignmentEditor();
    Q_INVOKABLE void setEditorTriggerKind(const QString& kind);
    Q_INVOKABLE void beginTriggerCapture(int step = 1);
    Q_INVOKABLE void cancelTriggerCapture();
    Q_INVOKABLE void setEditorGesture(const QString& kind, int tapCount, int holdMs);
    Q_INVOKABLE void saveAssignment();
    // A control the backends have actually delivered this session. The dialog
    // uses it to say "your controller never sent this button" instead of
    // guessing from a hardware list that is always out of date.
    Q_INVOKABLE bool controlWasObserved(const QString& controlId) const;
    void noteObservedControl(const QString& controlId);

    bool captureInput(const QString& deviceGroup, const QString& triggerCode,
                      const QString& displayLabel);
    // Fourth notice tier. Raised when a backend knows a physical button exists
    // but cannot deliver it as a logical control — the GameSir case, where the
    // pad's capture button never reaches us. No producer is wired yet; the
    // backend that can tell the difference arrives with the GameInput work.
    void reportUnsupportedInput(const QString& displayLabel);
    void setControllerProfile(const ControlId::DeviceProfile& profile);
    void setLastFiredAction(const QString& actionId);

public slots:
    void retranslate();

signals:
    void editorChanged();
    void deviceGroupChanged();
    void rowsChanged();
    void controllerSpecificChanged();
    void controllerProfileChanged();
    void captureChanged();
    void conflictChanged();
    void compatibilityChanged();
    void relationNoticeChanged();
    void lastFiredActionChanged();

private:
    struct PendingChange {
        BindingResolver::Binding target;
        QVector<BindingResolver::Binding> conflicts;
        bool hasConversion = false;
        BindingResolver::Binding conversionSource;
        BindingResolver::Binding conversionTarget;
    };

    QString selectedProfile() const;
    void rebuildRows();
    PendingChange pendingChangeFor(const BindingResolver::Binding& target) const;
    bool conversionCanApply(const BindingResolver::Binding& target,
                            const BindingResolver::Binding& press,
                            const QVector<BindingResolver::Binding>& effective,
                            BindingResolver::Binding* converted) const;
    bool applyChange(const PendingChange& change);
    bool persistRowsAtomically(const QVector<BindingOverrideRow>& rows);
    // True when this binding owns a Win32 global hotkey, i.e. the only rows the
    // OS half of the transaction applies to.
    bool isGlobalHotkey(const BindingResolver::Binding& binding) const;
    bool persist(const BindingOverrideRow& row);
    void reloadAndRefresh();
    QString formatTrigger(const BindingResolver::Binding& binding) const;
    QString formatBinding(const BindingResolver::Binding& binding) const;
    static QString formatGestureBadge(const BindingResolver::Binding& binding);
    void setRelationNotice(const QString& kindId, const QString& text);
    QString noticeTextFor(BindingRelation::Kind kind,
                          const BindingResolver::Binding& target,
                          const BindingResolver::Binding& partner,
                          const QString& displayLabel) const;
    QString conflictMessageFor(const BindingResolver::Binding& target,
                               const BindingResolver::Binding& partner,
                               const QString& displayLabel) const;
    QString compatibilityMessageFor(const PendingChange& change,
                                    const QString& displayLabel) const;
    static QString gestureLabel(const BindingResolver::Binding& binding);
    static QString scopeLabel(ActionCatalog::Scope scope);
    bool editorCaptureInput(const QString& deviceGroup, const QString& triggerCode);
    void refreshEditorNotice();
    TriggerSpec editorTrigger() const;
    BindingResolver::Binding editorBinding() const;
    QString controlLabel(const QString& controlId) const;

    CaptureDatabase* m_database = nullptr;
    BindingRuntime* m_runtime = nullptr;
    std::function<void()> m_reloadRuntime;
    HotkeyApply m_hotkeyApply;
    PersistRow m_persistRow;
    QString m_deviceGroup = QStringLiteral("controller");
    QVariantList m_rows;
    bool m_controllerSpecific = false;
    QString m_controllerFingerprint;
    QString m_controllerName;
    ControlId::ControllerFamily m_controllerFamily = ControlId::ControllerFamily::Generic;
    bool m_captureActive = false;
    QString m_captureActionId;
    int m_captureSlot = 1;
    QString m_capturePrompt;
    bool m_conflictPending = false;
    QString m_conflictMessage;
    bool m_compatibilityPending = false;
    QString m_compatibilityMessage;
    QString m_relationNotice;
    QString m_relationKind = QStringLiteral("none");
    QString m_validationError;
    PendingChange m_pending;
    QString m_lastFiredAction = QStringLiteral("No action fired yet");

    bool m_editorOpen = false;
    QString m_editorActionId;
    QString m_editorActionLabel;
    QString m_editorScopeLabel;
    int m_editorSlot = 1;
    QString m_editorTriggerKind = QStringLiteral("single");
    QString m_editorCaptureStep = QStringLiteral("idle");
    QString m_editorFirstControl;
    QString m_editorSecondControl;
    GestureSpec m_editorGesture;
    QString m_editorNotice;
    QString m_editorNoticeKind = QStringLiteral("none");
    QSet<QString> m_observedControls;
};
