#include "MediaSessionPrototype.h"

#include <QSignalSpy>
#include <QtTest>

using namespace MediaSessionPrototype;
using namespace Qt::StringLiterals;

namespace {

class FakeService final : public IMediaSessionService
{
public:
    void subscribe(quint64 generation) override { subscription = generation; ++subscriptions; }
    void unsubscribe() override { subscription = 0; ++unsubscriptions; }
    void request(quint64 requestId, const QString& sessionId, Command command) override
    {
        lastRequest = requestId;
        target = sessionId;
        lastCommand = command;
        ++requests;
    }
    void cancel(quint64 requestId) override
    {
        cancelled.append(requestId);
        emit commandFinished(requestId, true);
    }
    void publish(const QList<Session>& sessions)
    {
        emit sessionsChanged(subscription, sessions);
    }

    quint64 subscription = 0;
    quint64 lastRequest = 0;
    int subscriptions = 0;
    int unsubscriptions = 0;
    int requests = 0;
    QString target;
    Command lastCommand = Command::Pause;
    QList<quint64> cancelled;
};

Session player(QString id, Command command = Command::Play)
{
    return {id, u"Fixture.App"_s, u"Fixture media"_s, u"Fixture artist"_s, command};
}

} // namespace

class MediaSessionPrototypeTest : public QObject
{
    Q_OBJECT

private slots:
    void requiresExplicitSelectionAndSupportedCommand()
    {
        FakeService service;
        Client client(service);
        client.activate();
        QVERIFY(!client.send(Command::Play));
        service.publish({player(u"tab-a"_s), player(u"tab-b"_s, Command::Next)});
        QVERIFY(client.selectedId().isEmpty());
        QVERIFY(!client.select(u"invalid"_s));
        QVERIFY(client.select(u"tab-b"_s));
        QVERIFY(!client.send(Command::Play));
        QCOMPARE(service.requests, 0);

        QSignalSpy completed(&client, &Client::commandFinished);
        QVERIFY(client.send(Command::Next));
        QCOMPARE(service.target, u"tab-b"_s);
        QCOMPARE(service.lastCommand, Command::Next);
        QVERIFY(!client.send(Command::Next));
        emit service.commandFinished(service.lastRequest, false);
        QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.at(0).at(0).toBool(), false);
    }

    void switchingAndDisappearanceCancelOldCommands()
    {
        FakeService service;
        Client client(service);
        client.activate();
        const Session first = player(u"first"_s);
        const Session second = player(u"second"_s);
        service.publish({first, second});
        QVERIFY(client.select(first.id));
        QVERIFY(client.send(Command::Play));
        const quint64 firstRequest = service.lastRequest;
        QSignalSpy completed(&client, &Client::commandFinished);
        QVERIFY(client.select(second.id));
        QVERIFY(client.send(Command::Play));
        const quint64 secondRequest = service.lastRequest;
        emit service.commandFinished(firstRequest, true);
        QCOMPARE(completed.size(), 0);

        service.publish({first});
        QVERIFY(client.selectedId().isEmpty());
        QCOMPARE(service.cancelled, QList<quint64>({firstRequest, secondRequest}));
        QVERIFY(!client.send(Command::Play));
        emit service.commandFinished(secondRequest, true);
        QCOMPARE(completed.size(), 0);
        service.publish({});
        QVERIFY(client.sessions().isEmpty());
    }

    void hidingRejectsLateResultsAndUnsubscribes()
    {
        FakeService service;
        const Session fixture = player(u"fixture"_s);
        {
            Client client(service);
            QVERIFY(!client.send(Command::Play));
            client.activate();
            client.activate();
            QCOMPARE(service.subscriptions, 1);
            const quint64 generation = service.subscription;
            service.publish({fixture});
            QVERIFY(client.select(fixture.id));
            QVERIFY(client.send(Command::Play));
            QSignalSpy completed(&client, &Client::commandFinished);
            client.deactivate();
            client.deactivate();
            QCOMPARE(service.unsubscriptions, 1);
            QVERIFY(client.selectedId().isEmpty());
            QVERIFY(client.sessions().isEmpty());
            QVERIFY(!client.send(Command::Play));
            client.activate();
            emit service.sessionsChanged(generation, {fixture});
            emit service.commandFinished(service.lastRequest, true);
            QVERIFY(client.sessions().isEmpty());
            QCOMPARE(completed.size(), 0);
        }
        QCOMPARE(service.unsubscriptions, 2);
        QCOMPARE(service.subscription, 0);
        QCOMPARE(service.cancelled.size(), 1);
    }

    void retainedMetadataIsBoundedAndCopied()
    {
        FakeService service;
        Client client(service);
        client.activate();
        QList<Session> many;
        for (int i = 0; i < Client::maxSessions + 3; ++i) {
            Session session = player(QString::number(i));
            session.title = QString(500, u'x');
            many.append(session);
        }
        service.publish(many);
        many[0].title.clear();
        QCOMPARE(client.sessions().size(), Client::maxSessions);
        QCOMPARE(client.sessions().first().title.size(), Client::maxTextLength);
    }
};

QTEST_GUILESS_MAIN(MediaSessionPrototypeTest)
#include "tst_mediasessionprototype.moc"
