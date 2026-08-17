#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace AnyKeep::XmppXmlLog {

inline bool isEnabled() { return qEnvironmentVariableIntValue("ANYKEEP_XMPP_XML_LOG") > 0; }

inline QString sanitized(QString xml)
{
    if (xml.contains(QStringLiteral("<bundle")) || xml.contains(QStringLiteral("<prekeys"))
        || xml.contains(QRegularExpression(QStringLiteral("<pk\\s")))) {
        const auto  itemMatch = QRegularExpression(QStringLiteral("<item\\s[^>]*id=['\"]([^'\"]+)['\"]")).match(xml);
        const auto  identityMatch = QRegularExpression(QStringLiteral("<ik\\b[^>]*>([^<]+)</ik>")).match(xml);
        QStringList details;
        if (itemMatch.hasMatch())
            details.append(QStringLiteral("device=%1").arg(itemMatch.captured(1)));
        if (identityMatch.hasMatch())
            details.append(QStringLiteral("identity-key=%1").arg(identityMatch.captured(1)));
        return details.isEmpty()
            ? QStringLiteral("[OMEMO public prekeys omitted]")
            : QStringLiteral("[OMEMO bundle %1; public prekeys omitted]").arg(details.join(QLatin1Char(' ')));
    }
    if (xml.contains(QRegularExpression(QStringLiteral("<encrypted\\b[^>]*xmlns=['\"]urn:xmpp:omemo:2['\"]")))) {
        const auto sid
            = QRegularExpression(QStringLiteral("<header\\s[^>]*sid=['\"]([^'\"]+)['\"]")).match(xml).captured(1);
        QStringList recipients;
        auto        keys = QRegularExpression(QStringLiteral("<key\\s([^>]*)>")).globalMatch(xml);
        while (keys.hasNext()) {
            const auto attributes = keys.next().captured(1);
            const auto rid
                = QRegularExpression(QStringLiteral("rid=['\"]([^'\"]+)['\"]")).match(attributes).captured(1);
            const bool kex = QRegularExpression(QStringLiteral("kex=['\"]true['\"]")).match(attributes).hasMatch();
            if (!rid.isEmpty())
                recipients.append(QStringLiteral("%1%2").arg(rid, kex ? QStringLiteral("/kex") : QString {}));
        }
        return QStringLiteral("[OMEMO encrypted sid=%1 recipients=%2]")
            .arg(sid.isEmpty() ? QStringLiteral("?") : sid,
                 recipients.isEmpty() ? QStringLiteral("?") : recipients.join(QLatin1Char(',')));
    }
    static const QRegularExpression sensitiveElement(
        QStringLiteral("(<(?:auth|response|encrypted)\\b[^>]*>).*?(</(?:auth|response|encrypted)>)"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    xml.replace(sensitiveElement, QStringLiteral("\\1[redacted]\\2"));
    return xml;
}

} // namespace AnyKeep::XmppXmlLog
