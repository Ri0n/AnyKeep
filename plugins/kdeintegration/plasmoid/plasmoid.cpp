#include "plasmoid.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <QLoggingCategory>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>
#include <QWindow>
#include <utility>

#include <KWaylandExtras>
#include <KWindowSystem>

#include "anykeep_config.h"

namespace {
constexpr auto ServiceName   = "com.github.ri0n.AnyKeep";
constexpr auto ObjectPath    = "/AnyKeep";
constexpr auto InterfaceName = "com.github.ri0n.AnyKeep";

Q_LOGGING_CATEGORY(logPlasmoid, "anykeep.plasmoid")

QDBusPendingCall callBackendWithoutActivation(const QString &method, const QVariantList &arguments = {})
{
    auto message = QDBusMessage::createMethodCall(QLatin1String(ServiceName), QLatin1String(ObjectPath),
                                                  QLatin1String(InterfaceName), method);
    message.setArguments(arguments);
    message.setAutoStartService(false);
    return QDBusConnection::sessionBus().asyncCall(message);
}

QDBusPendingCallWatcher *requestBackendActivation(QObject *parent)
{
#ifdef ANYKEEP_DEVEL_EXECUTABLE
    Q_UNUSED(parent);
    qCInfo(logPlasmoid) << "AnyKeep D-Bus activation is disabled in development builds";
    return nullptr;
#else
    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
    message.setArguments({ QLatin1String(ServiceName), quint32(0) });
    return new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), parent);
#endif
}

void loadTranslations()
{
    static bool loaded = false;
    if (std::exchange(loaded, true))
        return;

    QSettings     settings(QStringLiteral("R-Soft"), QStringLiteral("AnyKeep"));
    const QString forcedLangName = settings.value(QLatin1String("language")).toString();
    const bool    autoLang       = forcedLangName.isEmpty() || forcedLangName == QLatin1String("auto");
    const QLocale locale         = autoLang ? QLocale::system() : QLocale(forcedLangName);

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QLatin1String("/R-Soft/AnyKeep/translations");
    QStringList langDirs {
        QStringLiteral(TRANSLATIONSDIR),
        dataDir,
    };
#ifdef ANYKEEP_DEVEL_TRANSLATIONSDIR
    langDirs.prepend(QStringLiteral(ANYKEEP_DEVEL_TRANSLATIONSDIR));
#endif

    auto *translator = new QTranslator(qApp);
    for (const QString &langDir : langDirs) {
        if (translator->load(locale, QStringLiteral(APPNAME), QStringLiteral("_"), langDir)) {
            qApp->installTranslator(translator);
            return;
        }
    }

    delete translator;
}
}

NotesModel::NotesModel(QObject *parent) : QAbstractListModel(parent)
{
    loadTranslations();

    m_queryRefreshTimer = new QTimer(this);
    m_queryRefreshTimer->setSingleShot(true);
    m_queryRefreshTimer->setInterval(250);
    connect(m_queryRefreshTimer, &QTimer::timeout, this, &NotesModel::refresh);

    auto bus         = QDBusConnection::sessionBus();
    m_serviceWatcher = new QDBusServiceWatcher(
        QLatin1String(ServiceName), bus,
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);

    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered, this, &NotesModel::serviceRegistered);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &NotesModel::serviceUnregistered);
    bus.connect(QLatin1String(ServiceName), QLatin1String(ObjectPath), QLatin1String(InterfaceName),
                QStringLiteral("notesChanged"), this, SLOT(refresh()));

    const auto reply = bus.interface()->isServiceRegistered(QLatin1String(ServiceName));
    setAvailable(reply.isValid() && reply.value());
    if (m_available) {
        createInterface();
        refresh();
    }
}

int NotesModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_items.size(); }

QVariant NotesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};

    const auto &item = m_items.at(index.row());
    switch (role) {
    case NoteIdRole:
        return item.id;
    case StorageIdRole:
        return item.storageId;
    case TitleRole:
        return item.title;
    case ModifiedRole:
        return item.modified;
    default:
        return {};
    }
}

QHash<int, QByteArray> NotesModel::roleNames() const
{
    return {
        { NoteIdRole, "noteId" },
        { StorageIdRole, "storageId" },
        { TitleRole, "title" },
        { ModifiedRole, "modified" },
    };
}

bool    NotesModel::available() const { return m_available; }
bool    NotesModel::loading() const { return m_loading; }
bool    NotesModel::loadingMore() const { return m_loadingMore; }
bool    NotesModel::hasMore() const { return m_hasMore; }
QString NotesModel::query() const { return m_query; }

void NotesModel::setQuery(const QString &query)
{
    const QString normalizedQuery = query.trimmed();
    if (m_query == normalizedQuery)
        return;

    m_query = normalizedQuery;
    emit queryChanged();

    ++m_requestSerial;
    if (m_available && m_interface)
        setLoading(true);
    setLoadingMore(false);
    m_queryRefreshTimer->start();
}

void NotesModel::refresh()
{
    m_queryRefreshTimer->stop();
    if (!m_available || !m_interface)
        return;

    requestPage(0, false);
}

void NotesModel::loadMore()
{
    if (!m_available || !m_interface)
        return;
    if (!m_hasMore || m_loading || m_loadingMore)
        return;

    requestPage(m_items.size(), true);
}

bool NotesModel::parseNotesResponse(const QString &response, QList<Item> *items, bool *hasMore) const
{
    QJsonParseError error;
    const auto      document = QJsonDocument::fromJson(response.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(logPlasmoid) << "Invalid notes response:" << error.errorString();
        return false;
    }

    const auto object     = document.object();
    const auto notesValue = object.value(QStringLiteral("notes"));
    if (!notesValue.isArray()) {
        qCWarning(logPlasmoid) << "Invalid notes response: missing notes array";
        return false;
    }

    const auto notes = notesValue.toArray();
    items->clear();
    items->reserve(notes.size());
    for (const auto &value : notes) {
        const auto object = value.toObject();
        Item       item {
            object.value(QStringLiteral("id")).toString(),
            object.value(QStringLiteral("storageId")).toString(),
            object.value(QStringLiteral("title")).toString(),
            object.value(QStringLiteral("modified")).toString(),
        };
        if (!item.id.isEmpty() && !item.storageId.isEmpty())
            items->append(std::move(item));
    }
    *hasMore = object.value(QStringLiteral("hasMore")).toBool(false);
    return true;
}

void NotesModel::requestPage(int offset, bool append)
{
    if (append) {
        setLoadingMore(true);
    } else {
        setLoading(true);
        setLoadingMore(false);
    }

    const quint64 requestSerial = ++m_requestSerial;
    auto         *watcher       = new QDBusPendingCallWatcher(
        callBackendWithoutActivation(QStringLiteral("notesJson"), { offset, m_pageSize, m_query }), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, requestSerial, append]() {
        const QDBusPendingReply<QString> reply = *watcher;
        watcher->deleteLater();
        if (requestSerial != m_requestSerial)
            return;

        if (append)
            setLoadingMore(false);
        else
            setLoading(false);

        if (reply.isError()) {
            qCWarning(logPlasmoid) << "Failed to fetch notes:" << reply.error().message();
            setHasMore(false);
            return;
        }

        QList<Item> items;
        bool        hasMore = false;
        if (!parseNotesResponse(reply.value(), &items, &hasMore)) {
            setHasMore(false);
            return;
        }

        if (append) {
            if (!items.isEmpty()) {
                const int first = m_items.size();
                beginInsertRows({}, first, first + items.size() - 1);
                for (auto &item : items)
                    m_items.append(std::move(item));
                endInsertRows();
                emit countChanged();
            }
        } else {
            beginResetModel();
            m_items = std::move(items);
            endResetModel();
            emit countChanged();
        }

        setHasMore(hasMore);
    });
}

void NotesModel::openNote(int row, QWindow *activationWindow)
{
    if (!m_available || !m_interface || row < 0 || row >= m_items.size())
        return;
    const auto &item = m_items.at(row);
    callWithActivationToken(QStringLiteral("openNote"), { item.storageId, item.id }, activationWindow);
}

void NotesModel::createNote(QWindow *activationWindow)
{
    callWithActivationToken(QStringLiteral("createNote"), {}, activationWindow);
}
void NotesModel::showNoteManager(QWindow *activationWindow)
{
    callWithActivationToken(QStringLiteral("showNoteManager"), {}, activationWindow);
}
void NotesModel::showOptions(QWindow *activationWindow)
{
    callWithActivationToken(QStringLiteral("showOptions"), {}, activationWindow);
}
void NotesModel::showAbout(QWindow *activationWindow)
{
    callWithActivationToken(QStringLiteral("showAbout"), {}, activationWindow);
}
void NotesModel::quit()
{
    m_pendingCall.clear();
    call(QStringLiteral("quit"));
}

void NotesModel::serviceRegistered()
{
    createInterface();
    m_starting = false;
    setAvailable(true);
    refresh();
    runPendingCall();
}

void NotesModel::serviceUnregistered()
{
    ++m_requestSerial;
    m_starting = false;
    m_pendingCall.clear();
    m_queryRefreshTimer->stop();
    setLoading(false);
    setLoadingMore(false);
    setHasMore(false);
    setAvailable(false);
    delete m_interface;
    m_interface = nullptr;
    if (!m_items.isEmpty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
        emit countChanged();
    }
}

void NotesModel::setAvailable(bool available)
{
    if (m_available == available)
        return;
    m_available = available;
    emit availableChanged();
}

void NotesModel::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void NotesModel::setLoadingMore(bool loadingMore)
{
    if (m_loadingMore == loadingMore)
        return;
    m_loadingMore = loadingMore;
    emit loadingMoreChanged();
}

void NotesModel::setHasMore(bool hasMore)
{
    if (m_hasMore == hasMore)
        return;
    m_hasMore = hasMore;
    emit hasMoreChanged();
}

void NotesModel::call(const QString &method)
{
    if (m_available && m_interface)
        callBackendWithoutActivation(method);
}

void NotesModel::callWithActivationToken(const QString &method, const QVariantList &arguments,
                                         QWindow *activationWindow)
{
    if (!m_available || !m_interface) {
        if (arguments.isEmpty())
            callOrStart(method);
        return;
    }

    auto callMethod = [this, method, arguments]() {
        if (!m_available || !m_interface)
            return;

        callBackendWithoutActivation(method, arguments);
    };

    if (!activationWindow || !KWindowSystem::isPlatformWayland()) {
        callMethod();
        return;
    }

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, callMethod]() {
        const QString token = watcher->result();
        watcher->deleteLater();

        if (!token.isEmpty() && m_available && m_interface)
            callBackendWithoutActivation(QStringLiteral("setXdgActivationToken"), { token });

        callMethod();
    });
    watcher->setFuture(KWaylandExtras::xdgActivationToken(activationWindow, QStringLiteral("anykeep")));
}

bool NotesModel::requestBackend()
{
    if (m_available || m_starting)
        return true;

    auto *watcher = requestBackendActivation(this);
    if (!watcher)
        return false;

    m_starting = true;
    setLoading(true);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        const QDBusPendingReply<quint32> reply = *watcher;
        watcher->deleteLater();
        if (!reply.isError() || m_available)
            return;

        qCWarning(logPlasmoid) << "Failed to activate AnyKeep:" << reply.error().message();
        m_starting = false;
        m_pendingCall.clear();
        setLoading(false);
    });

    QTimer::singleShot(10000, this, [this]() {
        if (m_available)
            return;

        m_starting = false;
        m_pendingCall.clear();
        setLoading(false);
    });
    return true;
}

void NotesModel::activate() { requestBackend(); }

void NotesModel::callOrStart(const QString &method)
{
    if (m_available && m_interface) {
        callBackendWithoutActivation(method);
        return;
    }

    m_pendingCall = method;
    if (!requestBackend())
        m_pendingCall.clear();
}

void NotesModel::runPendingCall()
{
    if (m_pendingCall.isEmpty() || !m_available || !m_interface)
        return;

    const QString method = std::exchange(m_pendingCall, QString());
    callBackendWithoutActivation(method);
}

void NotesModel::createInterface()
{
    delete m_interface;
    m_interface = new QDBusInterface(QLatin1String(ServiceName), QLatin1String(ObjectPath),
                                     QLatin1String(InterfaceName), QDBusConnection::sessionBus(), this);
}

StickyNoteModel::StickyNoteModel(QObject *parent) : QObject(parent)
{
    auto bus         = QDBusConnection::sessionBus();
    m_serviceWatcher = new QDBusServiceWatcher(
        QLatin1String(ServiceName), bus,
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered, this, &StickyNoteModel::serviceRegistered);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &StickyNoteModel::serviceUnregistered);
    bus.connect(QLatin1String(ServiceName), QLatin1String(ObjectPath), QLatin1String(InterfaceName),
                QStringLiteral("stickyNotesChanged"), this, SLOT(refresh()));

    const auto reply = bus.interface()->isServiceRegistered(QLatin1String(ServiceName));
    setAvailable(reply.isValid() && reply.value());
    if (m_available)
        createInterface();
}

void StickyNoteModel::setPresentationId(const QString &presentationId)
{
    if (m_presentationId == presentationId)
        return;
    m_presentationId = presentationId;
    emit presentationIdChanged();
    refresh();
}

void StickyNoteModel::refresh()
{
    if (!m_available || !m_interface || m_presentationId.isEmpty()) {
        clear();
        return;
    }

    const quint64 serial  = ++m_requestSerial;
    auto         *watcher = new QDBusPendingCallWatcher(
        callBackendWithoutActivation(QStringLiteral("stickyNoteForPresentationJson"), { m_presentationId }), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serial]() {
        const QDBusPendingReply<QString> reply = *watcher;
        watcher->deleteLater();
        if (serial != m_requestSerial)
            return;
        if (reply.isError()) {
            qCWarning(logPlasmoid) << "Failed to fetch sticky note:" << reply.error().message();
            m_openWhenAvailable = false;
            clear();
            return;
        }

        QJsonParseError error;
        const auto      document = QJsonDocument::fromJson(reply.value().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            m_openWhenAvailable = false;
            clear();
            return;
        }
        const auto object   = document.object();
        const auto stickyId = object.value(QStringLiteral("stickyId")).toString();
        if (stickyId.isEmpty()) {
            m_openWhenAvailable = false;
            clear();
            return;
        }
        m_stickyId = stickyId;
        m_title    = object.value(QStringLiteral("title")).toString();
        m_body     = object.value(QStringLiteral("body")).toString();
        emit noteChanged();

        if (std::exchange(m_openWhenAvailable, false) && m_available && m_interface)
            callBackendWithoutActivation(QStringLiteral("openStickyNote"), { m_stickyId });
    });
}

void StickyNoteModel::open()
{
    if (m_available && m_interface && !m_stickyId.isEmpty()) {
        callBackendWithoutActivation(QStringLiteral("openStickyNote"), { m_stickyId });
        return;
    }

    if (m_presentationId.isEmpty())
        return;

    m_openWhenAvailable = true;
    if (!requestBackend())
        m_openWhenAvailable = false;
}

void StickyNoteModel::unpin()
{
    if (m_available && m_interface && !m_stickyId.isEmpty())
        callBackendWithoutActivation(QStringLiteral("unpinStickyNote"), { m_stickyId });
}

void StickyNoteModel::serviceRegistered()
{
    m_starting = false;
    createInterface();
    setAvailable(true);
    refresh();
}

void StickyNoteModel::serviceUnregistered()
{
    ++m_requestSerial;
    m_starting          = false;
    m_openWhenAvailable = false;
    delete m_interface;
    m_interface = nullptr;
    setAvailable(false);
    clear();
}

bool StickyNoteModel::requestBackend()
{
    if (m_available || m_starting)
        return true;

    auto *watcher = requestBackendActivation(this);
    if (!watcher)
        return false;

    m_starting = true;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        const QDBusPendingReply<quint32> reply = *watcher;
        watcher->deleteLater();
        if (!reply.isError() || m_available)
            return;

        qCWarning(logPlasmoid) << "Failed to activate AnyKeep for sticky note:" << reply.error().message();
        m_starting          = false;
        m_openWhenAvailable = false;
    });

    QTimer::singleShot(10000, this, [this]() {
        if (m_available)
            return;

        m_starting          = false;
        m_openWhenAvailable = false;
    });
    return true;
}

void StickyNoteModel::createInterface()
{
    delete m_interface;
    m_interface = new QDBusInterface(QLatin1String(ServiceName), QLatin1String(ObjectPath),
                                     QLatin1String(InterfaceName), QDBusConnection::sessionBus(), this);
}

void StickyNoteModel::clear()
{
    if (m_stickyId.isEmpty() && m_title.isEmpty() && m_body.isEmpty())
        return;
    m_stickyId.clear();
    m_title.clear();
    m_body.clear();
    emit noteChanged();
}

void StickyNoteModel::setAvailable(bool available)
{
    if (m_available == available)
        return;
    m_available = available;
    emit availableChanged();
}
