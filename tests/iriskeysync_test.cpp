#include "iris/iriskeysynctask.h"

#include <iris/xmpp_client.h>

#include <QDomDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace AnyKeep;

namespace {
QDomElement stanza(const QString &type, const QString &from, const QString &iqId, const QJsonObject &payload)
{
    QDomDocument document;
    auto         iq = document.createElement(QStringLiteral("iq"));
    iq.setAttribute(QStringLiteral("type"), type);
    iq.setAttribute(QStringLiteral("from"), from);
    iq.setAttribute(QStringLiteral("id"), iqId);
    auto element = document.createElementNS(IrisKeySyncTask::feature, QStringLiteral("key-sync"));
    element.appendChild(
        document.createTextNode(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))));
    iq.appendChild(element);
    document.appendChild(iq);
    return iq;
}
}

class IrisKeySyncTest : public QObject {
    Q_OBJECT

private slots:
    void rejectsPlaintextStorageKeyResponse()
    {
        XMPP::Client           client;
        IrisKeySyncRequestTask request(client.rootTask());
        const auto             peer = QStringLiteral("me@example.test/peer");
        request.request(XMPP::Jid(peer), IrisKeySyncRequestTask::RequestType::StorageKey, QStringLiteral("request-1"));
        const auto reply
            = stanza(QStringLiteral("result"), peer, request.id(),
                     { { QStringLiteral("type"), QStringLiteral("response") },
                       { QStringLiteral("requestId"), QStringLiteral("request-1") },
                       { QStringLiteral("recoveryKey"), QStringLiteral("plaintext-key-must-not-be-accepted") } });
        QVERIFY(static_cast<XMPP::Task *>(&request)->take(reply));
        QVERIFY(!request.success());
        QVERIFY(request.recoveryKey().isEmpty());
        QVERIFY(request.responseSenderKey().isEmpty());
        QVERIFY(request.statusString().contains(QStringLiteral("OMEMO")));
    }

    void acceptsMatchingTrustApproval()
    {
        XMPP::Client           client;
        IrisKeySyncRequestTask request(client.rootTask());
        const auto             peer = QStringLiteral("me@example.test/peer");
        request.request(XMPP::Jid(peer), IrisKeySyncRequestTask::RequestType::TrustBootstrap,
                        QStringLiteral("trust-1"));
        const auto reply = stanza(QStringLiteral("result"), peer, request.id(),
                                  { { QStringLiteral("type"), QStringLiteral("trust-approved") },
                                    { QStringLiteral("requestId"), QStringLiteral("trust-1") } });
        QVERIFY(static_cast<XMPP::Task *>(&request)->take(reply));
        QVERIFY(request.success());
        QVERIFY(request.trustApproved());
        QVERIFY(request.recoveryKey().isEmpty());
    }

    void duplicateRequestCannotReplaceApprovalIdentity()
    {
        XMPP::Client    client;
        IrisKeySyncTask receiver(&client);
        QSignalSpy      prompts(&receiver, &IrisKeySyncTask::trustRequestReceived);
        auto            request = [](const QString &from, const QByteArray &identity) {
            return stanza(QStringLiteral("set"), from, QStringLiteral("iq-1"),
                          { { QStringLiteral("type"), QStringLiteral("trust-request") },
                            { QStringLiteral("requestId"), QStringLiteral("same-request") },
                            { QStringLiteral("senderKey"), QString::fromLatin1(identity.toBase64()) } });
        };
        auto *task = static_cast<XMPP::Task *>(&receiver);
        QVERIFY(task->take(request(QStringLiteral("me@example.test/first"), QByteArrayLiteral("first-identity"))));
        QVERIFY(task->take(request(QStringLiteral("me@example.test/second"), QByteArrayLiteral("second-identity"))));
        QCOMPARE(prompts.size(), 1);
        QCOMPARE(prompts.first().at(1).toString(), QStringLiteral("me@example.test/first"));
        QCOMPARE(prompts.first().at(2).toByteArray(), QByteArrayLiteral("first-identity"));
        receiver.reject(QStringLiteral("same-request"));
        QVERIFY(task->take(request(QStringLiteral("me@example.test/second"), QByteArrayLiteral("second-identity"))));
        QCOMPARE(prompts.size(), 2);
    }
};

QTEST_GUILESS_MAIN(IrisKeySyncTest)
#include "iriskeysync_test.moc"
