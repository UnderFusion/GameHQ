// Structural invariants of the action catalog and the code-owned default
// bindings. These are the assumptions every other input test builds on but
// none pins directly: ids are unique, every default row points at a real
// action, no (group, action, slot) tuple is declared twice, and every default
// serializes to a pattern this build can parse back. A duplicated catalog row
// or default (exactly the pre-0.7.3 double global.toggle_desktop entry) fails
// here with the offending id in the message instead of surfacing as a
// duplicated entry in the Settings binding editor.

#include "input/ActionCatalog.h"
#include "input/BindingPattern.h"
#include "input/BindingResolver.h"

#include <QSet>
#include <QTest>

class ActionCatalogTest : public QObject
{
    Q_OBJECT

private slots:
    void catalogIdsAreNonEmptyAndUnique()
    {
        QSet<QString> seen;
        for (const ActionCatalog::Action& action : ActionCatalog::all()) {
            QVERIFY2(!action.id.isEmpty(), "catalog action with an empty id");
            QVERIFY2(!seen.contains(action.id),
                     qPrintable(QStringLiteral("duplicate catalog id: %1").arg(action.id)));
            seen.insert(action.id);
            QVERIFY2(!action.label.isEmpty(),
                     qPrintable(QStringLiteral("%1 has no label").arg(action.id)));
            QVERIFY2(!action.description.isEmpty(),
                     qPrintable(QStringLiteral("%1 has no description").arg(action.id)));
        }
    }

    void everyDefaultBindingReferencesACatalogAction()
    {
        for (const auto& binding : BindingResolver::defaultBindings()) {
            QVERIFY2(ActionCatalog::find(binding.actionId) != nullptr,
                     qPrintable(QStringLiteral("default binding for unknown action: %1")
                                    .arg(binding.actionId)));
        }
    }

    void defaultSlotTuplesAreUnique()
    {
        QSet<QString> seen;
        for (const auto& binding : BindingResolver::defaultBindings()) {
            const QString key = binding.deviceGroup + QLatin1Char('|')
                + binding.actionId + QLatin1Char('|') + QString::number(binding.slot);
            QVERIFY2(!seen.contains(key),
                     qPrintable(QStringLiteral("duplicate default binding: %1").arg(key)));
            seen.insert(key);
        }
    }

    void everyDefaultBindingParsesAsAValidPattern()
    {
        for (const auto& binding : BindingResolver::defaultBindings()) {
            QVERIFY2(!binding.triggerCode.isEmpty(),
                     qPrintable(QStringLiteral("default binding without a trigger: %1")
                                    .arg(binding.actionId)));
            QVERIFY2(!binding.unbound,
                     qPrintable(QStringLiteral("default binding declared unbound: %1")
                                    .arg(binding.actionId)));
            const auto parsed = BindingPattern::parse(binding.deviceGroup,
                                                      binding.triggerCode,
                                                      binding.activation,
                                                      binding.tapCount,
                                                      binding.holdMs);
            QVERIFY2(parsed.ok,
                     qPrintable(QStringLiteral("default binding %1 slot %2 does not parse: %3")
                                    .arg(binding.actionId).arg(binding.slot)
                                    .arg(parsed.error)));
        }
    }
};

QTEST_GUILESS_MAIN(ActionCatalogTest)
#include "tst_actioncatalog.moc"
