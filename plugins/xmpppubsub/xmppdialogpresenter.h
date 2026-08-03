#ifndef XMPPDIALOGPRESENTER_H
#define XMPPDIALOGPRESENTER_H

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QUrl>

#include <functional>
#include <utility>

class QQmlEngine;
class QQuickWindow;

namespace AnyKeep {

class XmppKeyResolutionController;

/** UI-neutral state object used by the QML trust confirmation dialog. */
class XmppKeySyncPrompt final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString fingerprint READ fingerprint CONSTANT)
    Q_PROPERTY(bool completed READ completed NOTIFY completedChanged)

public:
    explicit XmppKeySyncPrompt(QString fingerprint, QObject *parent = nullptr);

    QString fingerprint() const { return fingerprint_; }
    bool    completed() const { return completed_; }

    Q_INVOKABLE void accept();
    Q_INVOKABLE void reject();

signals:
    void completedChanged();
    void finished(bool accepted);

private:
    void finish(bool accepted);

    QString fingerprint_;
    bool    completed_ { false };
};

/**
 * @brief Presents XMPP-specific workflows through shared Qt Quick components.
 *
 * Android uses the application's active Qt Quick window. Desktop uses an
 * active Qt Quick window when available and creates a small standalone Quick
 * host when AnyKeep is currently represented only by its tray or QWidget shell.
 */
class XmppDialogPresenter final : public QObject {
    Q_OBJECT

public:
    explicit XmppDialogPresenter(QObject *parent = nullptr);
    ~XmppDialogPresenter() override;

    void presentKeyResolution(XmppKeyResolutionController *controller);
    void presentTrustRequest(QString requestId, QByteArray keyId, std::function<void()> approve,
                             std::function<void()> reject = {});
    void cancelAll();

private:
    struct TrustRequest {
        QString               requestId;
        QByteArray            keyId;
        std::function<void()> approve;
        std::function<void()> reject;
    };

    void          tryPresent();
    QObject      *createHost(const QUrl &componentUrl, QObject *controller);
    QQuickWindow *presentationWindow();
    QQmlEngine   *presentationEngine(QQuickWindow *window) const;
    void          releaseStandaloneWindowIfIdle();
    void          scheduleRetry();

    QPointer<XmppKeyResolutionController> resolutionController_;
    QPointer<QObject>                     resolutionHost_;
    QPointer<XmppKeySyncPrompt>           trustPrompt_;
    QPointer<QObject>                     trustHost_;
    QQueue<TrustRequest>                  trustRequests_;
    QPointer<QQuickWindow>                standaloneWindow_;
    QQmlEngine                           *standaloneEngine_ { nullptr };
    bool                                  retryScheduled_ { false };
    bool                                  hostCreationDeferred_ { false };
};

} // namespace AnyKeep

#endif // XMPPDIALOGPRESENTER_H
