#ifndef ANYKEEP_IRISJINGLEPUBLICATIONPROVIDER_H
#define ANYKEEP_IRISJINGLEPUBLICATIONPROVIDER_H

#include "mediareference.h"
#include "xmppdto.h"

#include <iris/jingle-pub.h>
#include <iris/xmpp_file-sharing.h>

#include <QHash>

namespace AnyKeep {

class IrisXmppBackend;

/** Durable local capability required to reproduce one published ciphertext. */
struct IrisJingleCapability {
    QString                            publicationId;
    QString                            itemId;
    QString                            from;
    QString                            node;
    QString                            noteId;
    QString                            contentRevision;
    MediaReference                     reference;
    XMPP::StatelessFileSharing::Cipher cipher { XMPP::StatelessFileSharing::Cipher::Unknown };
    QByteArray                         key;
    QByteArray                         iv;
    QByteArray                         cipherHash;
    quint64                            wireSize { 0 };

    bool isValid() const;
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
