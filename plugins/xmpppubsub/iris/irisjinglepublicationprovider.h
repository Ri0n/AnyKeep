#ifndef ANYKEEP_IRISJINGLEPUBLICATIONPROVIDER_H
#define ANYKEEP_IRISJINGLEPUBLICATIONPROVIDER_H

#include "mediachunkwire.h"
#include "mediareference.h"
#include "xmppdto.h"

#include <iris/jingle-pub.h>
#include <iris/xmpp_file-sharing.h>

#include <QHash>
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcIrisJingleMedia)

namespace AnyKeep {

class IrisXmppBackend;

enum class IrisJingleMediaRepresentation : quint8 {
    LegacyXep0448  = 1,
    ChunkedAnyKeep = 2,
};

/** Durable local capability required to reproduce one published ciphertext. */
struct IrisJingleCapability {
    QString                       publicationId;
    QString                       itemId;
    QString                       from;
    QString                       node;
    QString                       noteId;
    QString                       contentRevision;
    MediaReference                reference;
    IrisJingleMediaRepresentation representation { IrisJingleMediaRepresentation::LegacyXep0448 };

    // Legacy XEP-0448 whole-object representation.
    XMPP::StatelessFileSharing::Cipher cipher { XMPP::StatelessFileSharing::Cipher::Unknown };
    QByteArray                         key;
    QByteArray                         iv;

    // AnyKeep independently authenticated chunk representation.
    MediaChunkWireParameters chunked;

    /** SHA-256 of the wire object, or empty to advertise XEP-0300 hash-used. */
    QByteArray cipherHash;
    quint64    wireSize { 0 };

    QString invalidReason() const;
    bool    isValid() const { return invalidReason().isEmpty(); }
};

class IrisJinglePublicationProvider final : public XMPP::Jingle::PublishedSessionProvider {
public:
    struct PrepareResult {
        XMPP::Jingle::JinglePub publication;
        QString                 error;
    };

    IrisJinglePublicationProvider(IrisXmppBackend *backend, XmppConfig config, QString path, QByteArray encryptionKey,
                                  QObject *parent = nullptr);

    QString                        errorString() const { return error_; }
    PrepareResult                  prepare(IrisJingleCapability capability);
    QList<XMPP::Jingle::JinglePub> matchingPublications(const QByteArray &cipherHash, quint64 wireSize) const;
    QStringList                    publicationIdsForNote(const QString &noteId) const;
    bool                           removePublication(const QString &publicationId);
    void                           traceIncomingStart(const QString &xml) const;

protected:
    QList<XMPP::Jingle::PublishedSessionEndpoint> publishedSessionEndpoints() const override;
    void                                          restoreCachedPublishedSessions() override;
    void                                          synchronizePublishedSessions() override;
    void publishedSessionObserved(const XMPP::Jingle::PublishedSessionEndpoint &endpoint, const QString &itemId,
                                  const XMPP::Jingle::JinglePub &publication) override;
    void publishedSessionRetracted(const XMPP::Jingle::PublishedSessionEndpoint &endpoint,
                                   const QString                                &itemId) override;
    void publishedSessionNodeInvalidated(const XMPP::Jingle::PublishedSessionEndpoint &endpoint, bool deleted) override;

private:
    friend class IrisJinglePublicationProviderTest;

    XMPP::Jingle::JinglePub publication(const IrisJingleCapability &capability) const;
    bool                    cacheCapability(const IrisJingleCapability &capability);
    bool                    load();
    bool                    persist();
    QString observedKey(const XMPP::Jingle::PublishedSessionEndpoint &endpoint, const QString &itemId) const;

    IrisXmppBackend                        *backend_ = nullptr;
    XmppConfig                              config_;
    QString                                 path_;
    QByteArray                              encryptionKey_;
    QString                                 error_;
    bool                                    writable_ { true };
    QHash<QString, IrisJingleCapability>    capabilities_;
    QHash<QString, XMPP::Jingle::JinglePub> observed_;
};

} // namespace AnyKeep

#endif // ANYKEEP_IRISJINGLEPUBLICATIONPROVIDER_H
