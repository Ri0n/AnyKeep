#include "xmpperror.h"

#include <QAbstractSocket>
#include <QTest>
#include <QXmppAuthenticationError.h>
#include <QXmppError.h>
#include <QXmppGlobal.h>
#include <QXmppStanza.h>

using namespace AnyKeep;

class XmppErrorTest : public QObject {
    Q_OBJECT

private slots:
    void socketAndTimeoutErrorsAreTransient()
    {
        QCOMPARE(classifyXmppError({ QStringLiteral("offline"), QAbstractSocket::NetworkError }),
                 XmppErrorKind::Transient);
        QCOMPARE(classifyXmppError({ QStringLiteral("timeout"), QXmpp::TimeoutError {} }), XmppErrorKind::Transient);
    }

    void authenticationErrorsArePermanent()
    {
        const QXmpp::AuthenticationError authentication { QXmpp::AuthenticationError::NotAuthorized, {}, {} };
        QCOMPARE(classifyXmppError({ QStringLiteral("bad password"), authentication }), XmppErrorKind::Authentication);
        QCOMPARE(classifyXmppError({ QStringLiteral("not authorized"),
                                     QXmppStanza::Error(QXmppStanza::Error::Auth, QXmppStanza::Error::NotAuthorized) }),
                 XmppErrorKind::Authentication);
    }

    void waitStanzaErrorsAreTransient()
    {
        QCOMPARE(
            classifyXmppError({ QStringLiteral("try later"),
                                QXmppStanza::Error(QXmppStanza::Error::Wait, QXmppStanza::Error::UndefinedCondition) }),
            XmppErrorKind::Transient);
        QCOMPARE(classifyXmppError(
                     { QStringLiteral("server busy"),
                       QXmppStanza::Error(QXmppStanza::Error::Cancel, QXmppStanza::Error::ResourceConstraint) }),
                 XmppErrorKind::Transient);
    }

    void protocolAndTlsErrorsArePermanent()
    {
        QCOMPARE(classifyXmppError({ QStringLiteral("bad request"),
                                     QXmppStanza::Error(QXmppStanza::Error::Modify, QXmppStanza::Error::BadRequest) }),
                 XmppErrorKind::Protocol);
        QCOMPARE(classifyXmppError({ QStringLiteral("bad certificate"), QAbstractSocket::SslHandshakeFailedError }),
                 XmppErrorKind::Security);
    }
};

QTEST_GUILESS_MAIN(XmppErrorTest)

#include "xmpperror_test.moc"
