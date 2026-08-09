#include "xmppdialogpresenter.h"

#include "xmppkeyresolutioncontroller.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqml.h>

#include <utility>

namespace AnyKeep {
namespace {

    QQuickWindow *activeQuickWindow()
    {
        if (auto *window = qobject_cast<QQuickWindow *>(QGuiApplication::focusWindow())) {
            if (window->isVisible())
                return window;
        }
        for (auto *window : QGuiApplication::topLevelWindows()) {
            if (auto *quickWindow = qobject_cast<QQuickWindow *>(window); quickWindow && quickWindow->isVisible())
                return quickWindow;
        }
        return nullptr;
    }

    QQmlEngine *windowEngine(QQuickWindow *window)
    {
        if (!window)
            return nullptr;
        if (auto *context = QQmlEngine::contextForObject(window))
            return context->engine();
        if (auto *context = QQmlEngine::contextForObject(window->contentItem()))
            return context->engine();
        return qmlEngine(window->contentItem());
    }

} // namespace

XmppKeySyncPrompt::XmppKeySyncPrompt(QString fingerprint, QObject *parent) :
    QObject(parent), fingerprint_(std::move(fingerprint))
{
}

void XmppKeySyncPrompt::accept() { finish(true); }
void XmppKeySyncPrompt::reject() { finish(false); }

void XmppKeySyncPrompt::finish(bool accepted)
{
    if (completed_)
        return;
    completed_ = true;
    emit completedChanged();
    emit finished(accepted);
}

XmppDialogPresenter::XmppDialogPresenter(QObject *parent) : QObject(parent)
{
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this]() { tryPresent(); });
}

XmppDialogPresenter::~XmppDialogPresenter()
{
    if (standaloneWindow_)
        delete standaloneWindow_.data();
}

void XmppDialogPresenter::presentKeyResolution(XmppKeyResolutionController *controller)
{
    if (!controller)
        return;

    resolutionController_ = controller;
    connect(controller, &XmppKeyResolutionController::finished, this, [this](bool) {
        if (resolutionHost_)
            resolutionHost_->deleteLater();
        resolutionController_.clear();
        releaseStandaloneWindowIfIdle();
    });
    connect(controller, &QObject::destroyed, this, [this]() {
        resolutionController_.clear();
        if (resolutionHost_)
            resolutionHost_->deleteLater();
        releaseStandaloneWindowIfIdle();
    });
    tryPresent();
}

void XmppDialogPresenter::presentTrustRequest(QString requestId, QByteArray keyId, std::function<void()> approve,
                                              std::function<void()> reject)
{
    trustRequests_.enqueue({ std::move(requestId), std::move(keyId), std::move(approve), std::move(reject) });
    tryPresent();
}

void XmppDialogPresenter::cancelAll()
{
    if (resolutionController_ && !resolutionController_->completed())
        resolutionController_->abort();
    if (trustPrompt_ && !trustPrompt_->completed())
        trustPrompt_->reject();
    while (!trustRequests_.isEmpty()) {
        auto request = trustRequests_.dequeue();
        if (request.reject)
            request.reject();
    }
    if (resolutionHost_)
        resolutionHost_->deleteLater();
    if (trustHost_)
        trustHost_->deleteLater();
    releaseStandaloneWindowIfIdle();
}

void XmppDialogPresenter::tryPresent()
{
    if (resolutionController_ && !resolutionHost_) {
        resolutionHost_ = createHost(QUrl(QStringLiteral("qrc:/qml/XmppKeyResolutionHost.qml")), resolutionController_);
        if (resolutionHost_) {
            connect(resolutionHost_, &QObject::destroyed, this, [this]() {
                resolutionHost_.clear();
                tryPresent();
                releaseStandaloneWindowIfIdle();
            });
        } else if (!hostCreationDeferred_) {
            resolutionController_->abort();
        }
    }

    // A key-recovery workflow is modal and takes priority over inbound trust prompts.
    if (resolutionController_ || resolutionHost_ || trustPrompt_ || trustHost_ || trustRequests_.isEmpty()) {
        if ((!trustRequests_.isEmpty() || resolutionController_) && !resolutionHost_ && !trustHost_)
            scheduleRetry();
        else
            releaseStandaloneWindowIfIdle();
        return;
    }

    const auto request = trustRequests_.head();
    auto      *prompt  = new XmppKeySyncPrompt(QString::fromLatin1(request.keyId.toHex()), this);
    auto      *host    = createHost(QUrl(QStringLiteral("qrc:/qml/XmppKeySyncTrustHost.qml")), prompt);
    if (!host) {
        prompt->deleteLater();
        if (hostCreationDeferred_) {
            scheduleRetry();
        } else {
            trustRequests_.dequeue();
            if (request.reject)
                request.reject();
            QTimer::singleShot(0, this, [this]() { tryPresent(); });
        }
        return;
    }

    trustRequests_.dequeue();
    trustPrompt_ = prompt;
    trustHost_   = host;

    connect(prompt, &XmppKeySyncPrompt::finished, this, [this, request](bool accepted) mutable {
        if (accepted) {
            if (request.approve)
                request.approve();
        } else if (request.reject) {
            request.reject();
        }
        if (trustPrompt_)
            trustPrompt_->deleteLater();
        trustPrompt_.clear();
    });
    connect(host, &QObject::destroyed, this, [this]() {
        trustHost_.clear();
        if (trustPrompt_ && !trustPrompt_->completed())
            trustPrompt_->reject();
        tryPresent();
        releaseStandaloneWindowIfIdle();
    });
}

QObject *XmppDialogPresenter::createHost(const QUrl &componentUrl, QObject *controller)
{
    hostCreationDeferred_       = false;
    const bool preferStandalone = componentUrl.fileName() == QStringLiteral("XmppKeyResolutionHost.qml");
    auto      *window           = presentationWindow(preferStandalone);
    auto      *engine           = presentationEngine(window);
    if (!window || !engine || !window->contentItem()) {
        hostCreationDeferred_ = true;
        return nullptr;
    }

    QQmlComponent component(engine, componentUrl, this);
    if (component.status() != QQmlComponent::Ready) {
        qWarning().noquote() << "Could not load" << componentUrl.toString() << component.errorString();
        return nullptr;
    }

    QVariantMap initialProperties;
    initialProperties.insert(QStringLiteral("controller"), QVariant::fromValue(controller));
    initialProperties.insert(QStringLiteral("hostItem"), QVariant::fromValue(window->contentItem()));
    if (preferStandalone)
        initialProperties.insert(QStringLiteral("standalone"), window == standaloneWindow_);
    auto *host = component.createWithInitialProperties(initialProperties, engine->rootContext());
    if (!host) {
        qWarning().noquote() << "Could not create" << componentUrl.toString() << component.errorString();
        return nullptr;
    }
    host->setParent(this);
    connect(window, &QQuickWindow::visibleChanged, host, [host](bool visible) {
        if (!visible)
            host->deleteLater();
    });
    connect(window, &QObject::destroyed, host, [host]() { host->deleteLater(); });
    return host;
}

QQuickWindow *XmppDialogPresenter::presentationWindow(bool preferStandalone)
{
#ifdef Q_OS_ANDROID
    if (auto *window = activeQuickWindow(); window && windowEngine(window))
        return window;
    return nullptr;
#else
    if (!preferStandalone) {
        if (auto *window = activeQuickWindow(); window && windowEngine(window))
            return window;
    }

    if (!standaloneWindow_) {
        standaloneEngine_ = new QQmlEngine(this);
        auto *window      = new QQuickWindow;
        standaloneWindow_ = window;
        window->setTitle(tr("Repair XMPP note synchronization — AnyKeep"));
        window->setFlags(Qt::Dialog);
        window->setModality(Qt::ApplicationModal);
        if (auto *focusWindow = QGuiApplication::focusWindow())
            window->setTransientParent(focusWindow);
        window->resize(800, 620);
        window->setMinimumSize(QSize(320, 360));
        connect(window, &QQuickWindow::visibleChanged, this, [this](bool visible) {
            if (visible)
                return;
            if (resolutionController_ && !resolutionController_->completed()) {
                if (resolutionController_->currentPage() == XmppKeyResolutionController::ResultPage)
                    resolutionController_->next();
                else
                    resolutionController_->abort();
            }
            if (trustPrompt_ && !trustPrompt_->completed())
                trustPrompt_->reject();
            QTimer::singleShot(0, this, [this]() { releaseStandaloneWindowIfIdle(); });
        });
        connect(window, &QObject::destroyed, this, [this]() { standaloneWindow_.clear(); });
    }

    standaloneWindow_->show();
    standaloneWindow_->raise();
    standaloneWindow_->requestActivate();
    return standaloneWindow_;
#endif
}

QQmlEngine *XmppDialogPresenter::presentationEngine(QQuickWindow *window) const
{
    if (window && window == standaloneWindow_)
        return standaloneEngine_;
    return windowEngine(window);
}

void XmppDialogPresenter::releaseStandaloneWindowIfIdle()
{
#ifndef Q_OS_ANDROID
    if (resolutionController_ || resolutionHost_ || trustPrompt_ || trustHost_ || !trustRequests_.isEmpty())
        return;

    if (standaloneWindow_) {
        auto *window = standaloneWindow_.data();
        standaloneWindow_.clear();
        window->hide();
        window->deleteLater();
    }
    if (standaloneEngine_) {
        standaloneEngine_->deleteLater();
        standaloneEngine_ = nullptr;
    }
#endif
}

void XmppDialogPresenter::scheduleRetry()
{
    if (retryScheduled_)
        return;
    retryScheduled_ = true;
    QTimer::singleShot(250, this, [this]() {
        retryScheduled_ = false;
        tryPresent();
    });
}

} // namespace AnyKeep
