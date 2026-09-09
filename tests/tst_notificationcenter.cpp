#include "notify/NotificationCenter.h"
#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QtTest>

class TestNotificationCenter : public QObject
{
    Q_OBJECT
    static QVariant at(NotificationCenter& center, int row, int role) {
        auto* model = center.visibleToasts();
        return model->data(model->index(row, 0), role);
    }
private slots:
    void updatesTheSameRowAndPreservesLargeIds()
    {
        NotificationCenter center(nullptr);
        center.setVisibleLimit(4);
        QAbstractItemModelTester invariant(center.visibleToasts(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        const quint64 id = (quint64(1) << 60) + 19;
        center.post(id, "Requested");
        QPersistentModelIndex row(center.visibleToasts()->index(0, 0));
        QCOMPARE(at(center, 0, ToastModel::Key).toString(), QString("capture:%1").arg(id));
        QVERIFY(at(center, 0, ToastModel::Pending).toBool());
        QVERIFY(center.update(id, "Saved", "Game", {}, "success"));
        QVERIFY(row.isValid());
        QCOMPARE(center.visibleToasts()->rowCount(), 1);
        QCOMPARE(at(center, 0, ToastModel::Title).toString(), QString("Saved"));
        QVERIFY(!at(center, 0, ToastModel::Pending).toBool());
    }
    void duplicatePostsAndUpdatesDoNotRestartPresentation()
    {
        NotificationCenter center(nullptr);
        center.setVisibleLimit(4);
        QSignalSpy changed(center.visibleToasts(), &QAbstractItemModel::dataChanged);
        center.post(7, "Requested");
        const auto first = at(center, 0, ToastModel::Revision);
        center.post(7, "Requested");
        QCOMPARE(changed.count(), 0);
        QCOMPARE(at(center, 0, ToastModel::Revision), first);
        QVERIFY(center.update(7, "Saved"));
        QCOMPARE(changed.count(), 1);
        const auto terminal = at(center, 0, ToastModel::Revision);
        QVERIFY(center.update(7, "Saved"));
        center.post(7, "Requested"); // late duplicate receipt cannot regress success
        QCOMPARE(changed.count(), 1);
        QCOMPARE(at(center, 0, ToastModel::Revision), terminal);
        QCOMPARE(at(center, 0, ToastModel::Title).toString(), QString("Saved"));
    }
    void unknownAndEvictedUpdatesDoNotCreateToasts()
    {
        NotificationCenter center(nullptr);
        center.setVisibleLimit(2);
        QVERIFY(!center.update(90, "Saved"));
        QVERIFY(!center.update(0, "Saved"));
        QVERIFY(!center.update(90, "Screenshot failed", "Rejected", {}, "error"));
        QCOMPARE(center.visibleToasts()->rowCount(), 0);
        for (quint64 id = 1; id <= 3; ++id) center.post(id, "Requested");
        QVERIFY(!center.update(1, "Saved"));
        QCOMPARE(center.visibleToasts()->rowCount(), 2);
    }
    void staleDismissalCannotRemoveUpdatedToast()
    {
        NotificationCenter center(nullptr);
        center.setVisibleLimit(4);
        center.post(1, "Requested");
        const int old = at(center, 0, ToastModel::Revision).toInt();
        QVERIFY(center.update(1, "Screenshot failed", "Gate rejected this request",
                              {}, "error"));
        QCOMPARE(at(center, 0, ToastModel::Title).toString(), QString("Screenshot failed"));
        QCOMPARE(at(center, 0, ToastModel::Kind).toString(), QString("error"));
        QVERIFY(!at(center, 0, ToastModel::Pending).toBool());
        center.dismiss("capture:1", old);
        QCOMPARE(center.visibleToasts()->rowCount(), 1);
        center.dismiss("capture:1", at(center, 0, ToastModel::Revision).toInt());
        QCOMPARE(center.visibleToasts()->rowCount(), 0);
        QVERIFY(!center.update(1, "Saved"));
    }
    void twentyPostsStayWithinPresentationCap()
    {
        NotificationCenter center(nullptr);
        center.setVisibleLimit(4);
        QAbstractItemModelTester invariant(center.visibleToasts(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        for (quint64 id = 1; id <= 20; ++id) {
            if (id % 2) center.post(id, "Requested");
            else center.post("Ordinary notification");
            QVERIFY(center.visibleToasts()->rowCount() <= center.visibleLimit());
        }
        QCOMPARE(center.visibleToasts()->rowCount(), 4);
        center.setVisibleLimit(2);
        QCOMPARE(center.visibleToasts()->rowCount(), 2);
        center.setVisibleLimit(0);
        QCOMPARE(center.visibleToasts()->rowCount(), 0);
    }
};
QTEST_GUILESS_MAIN(TestNotificationCenter)
#include "tst_notificationcenter.moc"
