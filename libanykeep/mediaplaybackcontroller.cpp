#include "mediaplaybackcontroller.h"

#include "localmediaimageprovider.h"
#include "mediaplaybacksession.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "themediconimageprovider.h"

#include <QCoreApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>

#include <algorithm>

namespace AnyKeep {
namespace {
    // Its engine and window have application lifetime, independent of the note's
    // QQuickWidget/engine. The session lease is returned after the view is destroyed.
    class MediaWindowHost final : public QObject {
    public:
        MediaWindowHost() : QObject(QCoreApplication::instance())
        {
            installLocalMediaImageProvider(&engine);
            installThemedIconImageProvider(&engine);
        }
        ~MediaWindowHost() override { delete window; }
        QQmlEngine    engine;
        QQuickWindow *window = nullptr;
    };
}

class MediaPlaybackController::Impl {
public:
    Impl(MediaPlaybackController *owner, NoteEditor *editor) : owner(owner), editor(editor)
    {
        if (!editor)
            return;
        QObject::connect(editor, &NoteEditor::allViewsClosed, owner, [this] { release(); });
        QObject::connect(editor, &NoteEditor::mediaChanged, owner, [this](const auto &) {
            if (session && !session->matches(referenceFor(session->currentSourceUri())))
                release();
        });
        QObject::connect(editor->model(), &NoteBlockModel::contentsChanged, owner, [this] {
            if (!session)
                return;
            const auto uri = session->currentSourceUri();
            for (int row = 0; row < this->editor->model()->rowCount(); ++row) {
                if (this->editor->model()->data(this->editor->model()->index(row), NoteBlockModel::UrlRole) == uri)
                    return;
            }
            release();
        });
    }
    ~Impl() { release(); }

    MediaReference referenceFor(const QString &uri) const
    {
        if (editor) {
            for (const auto &reference : editor->media()) {
                if (reference.uri() == uri
                    && (reference.mediaType.startsWith(QLatin1String("audio/"))
                        || reference.mediaType.startsWith(QLatin1String("video/"))))
                    return reference;
            }
        }
        return {};
    }

    bool ensureSession(const QString &uri)
    {
        const auto reference = referenceFor(uri);
        if (!reference.isValid())
            return false;
        if (session && session->matches(reference))
            return true;
        release();
        session = MediaPlaybackSession::acquire(reference, owner);
        if (!session)
            return false;
        QObject::connect(session, &MediaPlaybackSession::stateChanged, owner, &MediaPlaybackController::stateChanged);
        for (const auto &output : std::as_const(outputs)) {
            if (output)
                session->attachVideoOutput(output);
        }
        return true;
    }

    void release()
    {
        if (!session)
            return;
        auto *previous = session.data();
        session.clear();
        QObject::disconnect(previous, nullptr, owner, nullptr);
        for (const auto &output : std::as_const(outputs)) {
            if (output)
                previous->detachVideoOutput(output);
        }
        previous->release(owner);
        emit owner->stateChanged();
    }

    MediaPlaybackController       *owner;
    QPointer<NoteEditor>           editor;
    QPointer<MediaPlaybackSession> session;
    QList<QPointer<QObject>>       outputs;
};

MediaPlaybackController::MediaPlaybackController(NoteEditor *editor, QObject *parent) :
    QObject(parent), impl_(std::make_unique<Impl>(this, editor))
{
}
MediaPlaybackController::~MediaPlaybackController() = default;

bool MediaPlaybackController::available() const
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return true;
#else
    return false;
#endif
}
QString MediaPlaybackController::currentSourceUri() const
{
    return impl_->session ? impl_->session->currentSourceUri() : QString();
}
bool    MediaPlaybackController::playing() const { return impl_->session && impl_->session->playing(); }
bool    MediaPlaybackController::loading() const { return impl_->session && impl_->session->loading(); }
bool    MediaPlaybackController::seeking() const { return impl_->session && impl_->session->seeking(); }
bool    MediaPlaybackController::hasVideoFrame() const { return impl_->session && impl_->session->hasVideoFrame(); }
qint64  MediaPlaybackController::position() const { return impl_->session ? impl_->session->position() : 0; }
qint64  MediaPlaybackController::duration() const { return impl_->session ? impl_->session->duration() : 0; }
QString MediaPlaybackController::errorString() const
{
    return impl_->session ? impl_->session->errorString() : QString();
}
bool MediaPlaybackController::play(const QString &uri)
{
    return impl_->ensureSession(uri) && impl_->session->play(uri);
}
bool MediaPlaybackController::toggle(const QString &uri)
{
    return impl_->ensureSession(uri) && impl_->session->toggle(uri);
}
void MediaPlaybackController::pause()
{
    if (impl_->session)
        impl_->session->pause();
}
void MediaPlaybackController::suspend()
{
    if (impl_->session)
        impl_->session->suspend();
}
void MediaPlaybackController::stop() { impl_->release(); }
bool MediaPlaybackController::seek(const QString &uri, qint64 positionMs)
{
    return impl_->ensureSession(uri) && impl_->session->seek(uri, positionMs);
}
void MediaPlaybackController::attachVideoOutput(QObject *output)
{
    if (!output)
        return;
    impl_->outputs.removeIf([output](const auto &item) { return !item || item == output; });
    impl_->outputs.append(output);
    if (impl_->session)
        impl_->session->attachVideoOutput(output);
}
void MediaPlaybackController::detachVideoOutput(QObject *output)
{
    impl_->outputs.removeIf([output](const auto &item) { return !item || item == output; });
    if (impl_->session)
        impl_->session->detachVideoOutput(output);
}

bool MediaPlaybackController::openWindow(const QString &uri, const QString &title, const QString &previewUrl,
                                         const QUrl &componentUrl)
{
    const auto reference = impl_->referenceFor(uri);
    if (!available() || !reference.isValid() || componentUrl.scheme() != QLatin1String("qrc"))
        return false;
    auto         *host    = new MediaWindowHost;
    auto         *session = MediaPlaybackSession::acquire(reference, host);
    QQmlComponent component(&host->engine, componentUrl);
    auto *object = component.createWithInitialProperties({ { QStringLiteral("playback"), QVariant::fromValue(session) },
                                                           { QStringLiteral("sourceUri"), uri },
                                                           { QStringLiteral("mediaTitle"), title },
                                                           { QStringLiteral("previewUrl"), QUrl(previewUrl) } });
    host->window = qobject_cast<QQuickWindow *>(object);
    if (!host->window) {
        qWarning() << "Could not create media window:" << component.errorString();
        delete object;
        delete host;
        return false;
    }
    QQmlEngine::setObjectOwnership(object, QQmlEngine::CppOwnership);
    connect(host->window, &QWindow::visibleChanged, host, [host](bool visible) {
        if (!visible)
            host->deleteLater();
    });
    host->window->show();
    host->window->requestActivate();
    if (session->currentSourceUri().isEmpty())
        session->play(uri);
    return true;
}
} // namespace AnyKeep
