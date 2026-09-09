// The identity that ties one capture press to its outcome in the log
// (docs/capture-engine.md, plan p1-2).
//
// Pure value logic: no capture pipeline, no audio or graphics device, so the
// rules that decide what a diagnostics chain looks like are pinned here rather
// than inferred from a log paste.
#include "capture/CaptureRequest.h"

#include <QSet>
#include <QtTest>

using Source = CaptureRequest::Source;

class CaptureRequestTest : public QObject
{
    Q_OBJECT
private slots:
    void aDefaultRequestIsNotAChain();
    void idsAreUniqueAndIncreasing();
    void deviceGroupsMapToTheSourceThatPressed();
    void anUnknownDeviceGroupIsNotGuessed();
    void everySourceHasAStableLabel();
    void theTagLeadsWithTheChainId();
};

void CaptureRequestTest::aDefaultRequestIsNotAChain()
{
    const CaptureRequest request;
    QVERIFY(!request.isValid());
    QCOMPARE(request.id, quint64(0));
    QCOMPARE(request.source, Source::Unknown);
    // A caller that forgot to mint one must not look like request 0 in the log.
    QVERIFY(!request.tag().startsWith(QLatin1Char('0')));
}

void CaptureRequestTest::idsAreUniqueAndIncreasing()
{
    QSet<quint64> seen;
    quint64 previous = 0;
    for (int i = 0; i < 32; ++i) {
        const CaptureRequest request = CaptureRequest::create(Source::Controller);
        QVERIFY(request.isValid());
        QVERIFY2(request.id > previous, "a later press must never reuse an earlier chain id");
        previous = request.id;
        QVERIFY(!seen.contains(request.id));
        seen.insert(request.id);
        QVERIFY(request.monotonicMs >= 0);
    }
}

void CaptureRequestTest::deviceGroupsMapToTheSourceThatPressed()
{
    QCOMPARE(CaptureRequest::sourceForDeviceGroup(QStringLiteral("controller")),
             Source::Controller);
    QCOMPARE(CaptureRequest::sourceForDeviceGroup(QStringLiteral("keyboard")), Source::Keyboard);
    QCOMPARE(CaptureRequest::sourceForDeviceGroup(QStringLiteral("mouse")), Source::Mouse);
}

void CaptureRequestTest::anUnknownDeviceGroupIsNotGuessed()
{
    // A new device group must read as unknown until it is mapped, rather than
    // silently reporting the wrong device in a bug report.
    QCOMPARE(CaptureRequest::sourceForDeviceGroup(QStringLiteral("gamepad")), Source::Unknown);
    QCOMPARE(CaptureRequest::sourceForDeviceGroup(QString()), Source::Unknown);
    QCOMPARE(CaptureRequest::sourceForDeviceGroup(QStringLiteral("Controller")), Source::Unknown);
}

void CaptureRequestTest::everySourceHasAStableLabel()
{
    const QVector<Source> sources{Source::Unknown, Source::Controller, Source::Keyboard,
                                  Source::Mouse,   Source::Overlay,    Source::Ui,
                                  Source::Tray};
    QSet<QString> labels;
    for (Source source : sources) {
        const QString label = CaptureRequest::label(source);
        QVERIFY2(!label.isEmpty(), "a source with no label makes the log ambiguous");
        QVERIFY2(!labels.contains(label), "two sources must not share a label");
        labels.insert(label);
    }
}

void CaptureRequestTest::theTagLeadsWithTheChainId()
{
    CaptureRequest request = CaptureRequest::create(Source::Controller);
    request.id = 12;
    request.monotonicMs = 4231;
    // Grepping "ReplaySave[12 " has to find this chain and no other.
    QCOMPARE(request.tag(), QStringLiteral("12 src=controller +4231ms"));
}

QTEST_APPLESS_MAIN(CaptureRequestTest)
#include "tst_capturerequest.moc"
