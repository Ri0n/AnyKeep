#include "xmpperror.h"

#include <QAbstractSocket>
#include <QNetworkReply>
#include <QXmppAuthenticationError.h>
#include <QXmppBindError.h>
#include <QXmppError.h>
#include <QXmppGlobal.h>
#include <QXmppStanza.h>
#include <QXmppStreamError.h>

namespace AnyKeep {
namespace {

    XmppErrorKind classifySocketError(QAbstractSocket::SocketError error)
    {
        switch (error) {
        case QAbstractSocket::ProxyAuthenticationRequiredError:
            return XmppErrorKind::Authentication;
        case QAbstractSocket::SocketAccessError:
        case QAbstractSocket::UnsupportedSocketOperationError:
            return XmppErrorKind::Configuration;
        case QAbstractSocket::SslHandshakeFailedError:
        case QAbstractSocket::SslInternalError:
        case QAbstractSocket::SslInvalidUserDataError:
            return XmppErrorKind::Security;
        default:
            return XmppErrorKind::Transient;
        }
    }

    XmppErrorKind classifyStanzaError(const QXmppStanza::Error &error)
    {
        if (error.type() == QXmppStanza::Error::Wait)
            return XmppErrorKind::Transient;

        switch (error.condition()) {
        case QXmppStanza::Error::InternalServerError:
        case QXmppStanza::Error::RecipientUnavailable:
        case QXmppStanza::Error::RemoteServerNotFound:
        case QXmppStanza::Error::RemoteServerTimeout:
        case QXmppStanza::Error::ResourceConstraint:
        case QXmppStanza::Error::ServiceUnavailable:
            return XmppErrorKind::Transient;
        case QXmppStanza::Error::Forbidden:
        case QXmppStanza::Error::NotAuthorized:
        case QXmppStanza::Error::RegistrationRequired:
        case QXmppStanza::Error::SubscriptionRequired:
            return XmppErrorKind::Authentication;
        case QXmppStanza::Error::PolicyViolation:
            return XmppErrorKind::Security;
        default:
            return XmppErrorKind::Protocol;
        }
    }

    XmppErrorKind classifyStreamError(QXmpp::StreamError error)
    {
        switch (error) {
        case QXmpp::StreamError::ConnectionTimeout:
        case QXmpp::StreamError::HostGone:
        case QXmpp::StreamError::InternalServerError:
        case QXmpp::StreamError::RemoteConnectionFailed:
        case QXmpp::StreamError::Reset:
        case QXmpp::StreamError::ResourceConstraint:
        case QXmpp::StreamError::SystemShutdown:
            return XmppErrorKind::Transient;
        case QXmpp::StreamError::NotAuthorized:
            return XmppErrorKind::Authentication;
        case QXmpp::StreamError::PolicyViolation:
        case QXmpp::StreamError::RestrictedXml:
            return XmppErrorKind::Security;
        default:
            return XmppErrorKind::Protocol;
        }
    }

} // namespace

XmppErrorKind classifyXmppError(const QXmppError &error)
{
    if (const auto value = error.value<QAbstractSocket::SocketError>())
        return classifySocketError(*value);
    if (error.value<QXmpp::TimeoutError>())
        return XmppErrorKind::Transient;
    if (error.value<QXmpp::AuthenticationError>())
        return XmppErrorKind::Authentication;
    if (const auto value = error.value<QXmpp::StreamError>())
        return classifyStreamError(*value);
    if (const auto value = error.value<QXmpp::BindError>())
        return classifyStanzaError(value->stanzaError);
    if (const auto value = error.value<QXmppStanza::Error>())
        return classifyStanzaError(*value);
    if (const auto value = error.value<QNetworkReply::NetworkError>()) {
        if (*value == QNetworkReply::AuthenticationRequiredError
            || *value == QNetworkReply::ProxyAuthenticationRequiredError) {
            return XmppErrorKind::Authentication;
        }
        if (*value == QNetworkReply::ProtocolInvalidOperationError || *value == QNetworkReply::ProtocolUnknownError) {
            return XmppErrorKind::Protocol;
        }
        return XmppErrorKind::Transient;
    }
    if (error.isNetworkError())
        return XmppErrorKind::Transient;
    return XmppErrorKind::Protocol;
}

} // namespace AnyKeep
