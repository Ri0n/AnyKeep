#ifndef ANYKEEP_XMPP_WORKER_PRIVATE_H
#define ANYKEEP_XMPP_WORKER_PRIVATE_H

#include "xmppdto.h"
#include "xmpperror.h"

#include <QXmppError.h>
#include <QXmppPubSubNodeConfig.h>
#include <QXmppPubSubPublishOptions.h>
#include <QXmppStanza.h>
#include <QXmppTrustLevel.h>

#include <variant>

namespace AnyKeep::XmppWorkerPrivate {

inline XmppTrustLevel backendTrustLevel(QXmpp::TrustLevel level)
{
    switch (level) {
    case QXmpp::TrustLevel::AutomaticallyTrusted:
        return XmppTrustLevel::AutomaticallyTrusted;
    case QXmpp::TrustLevel::ManuallyTrusted:
        return XmppTrustLevel::ManuallyTrusted;
    case QXmpp::TrustLevel::Authenticated:
        return XmppTrustLevel::Authenticated;
    case QXmpp::TrustLevel::AutomaticallyDistrusted:
    case QXmpp::TrustLevel::ManuallyDistrusted:
        return XmppTrustLevel::Distrusted;
    case QXmpp::TrustLevel::Undecided:
    default:
        return XmppTrustLevel::Undecided;
    }
}

inline QXmpp::TrustLevel qxmppTrustLevel(XmppTrustLevel level)
{
    switch (level) {
    case XmppTrustLevel::AutomaticallyTrusted:
        return QXmpp::TrustLevel::AutomaticallyTrusted;
    case XmppTrustLevel::ManuallyTrusted:
        return QXmpp::TrustLevel::ManuallyTrusted;
    case XmppTrustLevel::Authenticated:
        return QXmpp::TrustLevel::Authenticated;
    case XmppTrustLevel::Distrusted:
        // The backend-neutral model deliberately does not distinguish automatic
        // from explicit distrust. Converting back to QXmpp therefore preserves
        // the stronger, user-visible meaning.
        return QXmpp::TrustLevel::ManuallyDistrusted;
    case XmppTrustLevel::Undecided:
    default:
        return QXmpp::TrustLevel::Undecided;
    }
}

inline QXmppPubSubPublishOptions privatePublishOptions()
{
    QXmppPubSubPublishOptions options;
    options.setAccessModel(QXmppPubSubNodeConfig::Allowlist);
    options.setPersistItems(true);
    return options;
}

template <typename Result> const QXmppError *resultError(const Result &result)
{
    return std::get_if<QXmppError>(&result);
}

inline bool isItemNotFound(const QXmppError &error)
{
    const auto stanzaError = error.value<QXmppStanza::Error>();
    return (stanzaError && stanzaError->condition() == QXmppStanza::Error::ItemNotFound)
        || error.description.contains(QStringLiteral("No such item"), Qt::CaseInsensitive)
        || error.description.contains(QStringLiteral("item-not-found"), Qt::CaseInsensitive);
}

template <typename Result> void setXmppFailure(Result &result, const QXmppError &error, const QString &message)
{
    result.error     = message;
    result.errorKind = classifyXmppError(error);
}

template <typename Result> Result configurationChangedResult()
{
    Result result;
    result.error     = QStringLiteral("The XMPP configuration changed while the operation was running");
    result.errorKind = XmppErrorKind::Configuration;
    return result;
}

} // namespace AnyKeep::XmppWorkerPrivate

#endif // ANYKEEP_XMPP_WORKER_PRIVATE_H
