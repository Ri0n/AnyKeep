#ifndef ANYKEEP_XMPP_WORKER_PRIVATE_H
#define ANYKEEP_XMPP_WORKER_PRIVATE_H

#include "xmppdto.h"
#include "xmpperror.h"

#include <QXmppError.h>
#include <QXmppPubSubNodeConfig.h>
#include <QXmppPubSubPublishOptions.h>
#include <QXmppStanza.h>

#include <variant>

namespace AnyKeep::XmppWorkerPrivate {

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
