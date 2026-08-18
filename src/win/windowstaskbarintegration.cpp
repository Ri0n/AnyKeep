#include "windowstaskbarintegration.h"

#include "foldercatalogmanager.h"
#include "note.h"
#include "notemanager.h"
#include "notesindex.h"
#include "utils.h"

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLoggingCategory>
#include <QPlatformSurfaceEvent>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <array>
#include <string>

// clang-format off
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shobjidl.h>
// clang-format on

namespace AnyKeep {

namespace {

    Q_LOGGING_CATEGORY(logWindowsTaskbar, "anykeep.windows.taskbar")

    constexpr wchar_t UnpackagedAppId[]  = L"com.github.ri0n.AnyKeep";
    constexpr int     MaximumRecentNotes = 10;

    QWindow *noteWindow(QObject *object)
    {
        if (!object || !object->inherits("AnyKeep::NoteDialog"))
            return nullptr;
        return qobject_cast<QWindow *>(object);
    }

    QString hresultText(HRESULT result)
    {
        return QStringLiteral("0x%1").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    }

    QString nativeLaunchExecutable() { return QDir::toNativeSeparators(Utils::windowsLaunchExecutable()); }

    QString currentAppUserModelId()
    {
        if (!WindowsTaskbarIntegration::hasPackageIdentity())
            return QString::fromWCharArray(UnpackagedAppId);

        UINT32 length = 0;
        LONG   result = GetCurrentApplicationUserModelId(&length, nullptr);
        if (result != ERROR_INSUFFICIENT_BUFFER || length == 0)
            return {};

        std::wstring appId(length, L'\0');
        result = GetCurrentApplicationUserModelId(&length, appId.data());
        if (result != ERROR_SUCCESS)
            return {};
        if (!appId.empty() && appId.back() == L'\0')
            appId.pop_back();
        return QString::fromStdWString(appId);
    }

    HRESULT setStringProperty(IPropertyStore *store, REFPROPERTYKEY key, const QString &value)
    {
        if (!store)
            return E_POINTER;

        const std::wstring nativeValue = value.toStdWString();
        PROPVARIANT        propertyValue;
        HRESULT            hr = InitPropVariantFromString(nativeValue.c_str(), &propertyValue);
        if (FAILED(hr))
            return hr;

        hr = store->SetValue(key, propertyValue);
        PropVariantClear(&propertyValue);
        return hr;
    }

    QString quoteWindowsArgument(const QString &argument)
    {
        const bool needsQuoting = argument.isEmpty() || std::any_of(argument.cbegin(), argument.cend(), [](QChar ch) {
                                      return ch.isSpace() || ch == QLatin1Char('"');
                                  });
        if (!needsQuoting)
            return argument;

        QString quoted;
        quoted.reserve(argument.size() + 2);
        quoted += QLatin1Char('"');

        int backslashes = 0;
        for (const QChar ch : argument) {
            if (ch == QLatin1Char('\\')) {
                ++backslashes;
                continue;
            }
            if (ch == QLatin1Char('"')) {
                quoted += QString(backslashes * 2 + 1, QLatin1Char('\\'));
                quoted += ch;
                backslashes = 0;
                continue;
            }
            if (backslashes) {
                quoted += QString(backslashes, QLatin1Char('\\'));
                backslashes = 0;
            }
            quoted += ch;
        }
        if (backslashes)
            quoted += QString(backslashes * 2, QLatin1Char('\\'));
        quoted += QLatin1Char('"');
        return quoted;
    }

    QString commandLineArguments(const QStringList &arguments)
    {
        QStringList quoted;
        quoted.reserve(arguments.size());
        for (const auto &argument : arguments)
            quoted.append(quoteWindowsArgument(argument));
        return quoted.join(QLatin1Char(' '));
    }

    QString exportedJumpListIcon(const QString &resourcePath, const QString &baseName)
    {
        QFile resource(resourcePath);
        if (!resource.open(QIODevice::ReadOnly)) {
            qCWarning(logWindowsTaskbar) << "Failed to open Jump List icon resource" << resourcePath;
            return {};
        }

        const QByteArray sourceData = resource.readAll();
        QImage           image;
        if (!image.loadFromData(sourceData)) {
            qCWarning(logWindowsTaskbar) << "Failed to decode Jump List icon resource" << resourcePath;
            return {};
        }

        QByteArray pngData;
        QBuffer    buffer(&pngData);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
            qCWarning(logWindowsTaskbar) << "Failed to encode Jump List icon resource" << resourcePath;
            return {};
        }

        const QString iconDirectory = QDir(Utils::anykeepDataDir()).filePath(QStringLiteral("jump-list-icons"));
        if (!QDir().mkpath(iconDirectory)) {
            qCWarning(logWindowsTaskbar) << "Failed to create Jump List icon directory" << iconDirectory;
            return {};
        }

        const QString digest
            = QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex().left(12));
        const QString iconPath = QDir(iconDirectory).filePath(QStringLiteral("%1-%2.ico").arg(baseName, digest));

        if (!QFileInfo::exists(iconPath)) {
            QSaveFile file(iconPath);
            if (!file.open(QIODevice::WriteOnly)) {
                qCWarning(logWindowsTaskbar) << "Failed to create Jump List icon" << iconPath;
                return {};
            }

            QDataStream stream(&file);
            stream.setByteOrder(QDataStream::LittleEndian);

            // ICO header with one PNG-compressed image. Windows supports PNG
            // payloads in ICO files, while keeping the original Qt resources
            // as the single source of truth for the artwork.
            stream << quint16(0) << quint16(1) << quint16(1);

            const auto iconDimension = [](int size) { return static_cast<quint8>(size >= 256 ? 0 : size); };
            stream << iconDimension(image.width()) << iconDimension(image.height()) << quint8(0) << quint8(0)
                   << quint16(1) << quint16(32) << quint32(pngData.size()) << quint32(6 + 16);

            if (stream.writeRawData(pngData.constData(), pngData.size()) != pngData.size()
                || stream.status() != QDataStream::Ok || !file.commit()) {
                qCWarning(logWindowsTaskbar) << "Failed to write Jump List icon" << iconPath;
                return {};
            }
        }

        return QDir::toNativeSeparators(iconPath);
    }

    HRESULT createShellLink(const QStringList &arguments, const QString &title, const QString &description,
                            const QString &iconPath, IShellLinkW **result)
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;

        IShellLinkW *link = nullptr;
        HRESULT      hr   = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (FAILED(hr))
            return hr;

        const std::wstring executable = nativeLaunchExecutable().toStdWString();
        const std::wstring args       = commandLineArguments(arguments).toStdWString();
        const std::wstring tooltip    = description.toStdWString();
        const std::wstring itemTitle  = title.toStdWString();
        const std::wstring icon       = (iconPath.isEmpty() ? nativeLaunchExecutable() : iconPath).toStdWString();

        hr = link->SetPath(executable.c_str());
        if (SUCCEEDED(hr))
            hr = link->SetArguments(args.c_str());
        if (SUCCEEDED(hr))
            hr = link->SetIconLocation(icon.c_str(), 0);
        if (SUCCEEDED(hr) && !tooltip.empty())
            hr = link->SetDescription(tooltip.c_str());

        IPropertyStore *propertyStore = nullptr;
        if (SUCCEEDED(hr))
            hr = link->QueryInterface(IID_PPV_ARGS(&propertyStore));
        if (SUCCEEDED(hr)) {
            PROPVARIANT value;
            hr = InitPropVariantFromString(itemTitle.c_str(), &value);
            if (SUCCEEDED(hr)) {
                hr = propertyStore->SetValue(PKEY_Title, value);
                if (SUCCEEDED(hr))
                    hr = propertyStore->Commit();
                PropVariantClear(&value);
            }
        }
        if (propertyStore)
            propertyStore->Release();

        if (FAILED(hr)) {
            link->Release();
            return hr;
        }

        *result = link;
        return S_OK;
    }

    HRESULT createCollection(IObjectCollection **result)
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        return CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(result));
    }

    bool addShellLink(IObjectCollection *collection, const QStringList &arguments, const QString &title,
                      const QString &description = {}, const QString &iconPath = {})
    {
        if (!collection)
            return false;

        IShellLinkW  *link = nullptr;
        const HRESULT hr   = createShellLink(arguments, title, description, iconPath, &link);
        if (FAILED(hr)) {
            qCWarning(logWindowsTaskbar) << "Failed to create Jump List link:" << hresultText(hr);
            return false;
        }

        const HRESULT addResult = collection->AddObject(link);
        link->Release();
        if (FAILED(addResult)) {
            qCWarning(logWindowsTaskbar) << "Failed to add Jump List link:" << hresultText(addResult);
            return false;
        }
        return true;
    }

    QSet<QString> removedArguments(IObjectArray *removed)
    {
        QSet<QString> result;
        if (!removed)
            return result;

        UINT count = 0;
        if (FAILED(removed->GetCount(&count)))
            return result;

        for (UINT i = 0; i < count; ++i) {
            IShellLinkW *link = nullptr;
            if (FAILED(removed->GetAt(i, IID_PPV_ARGS(&link))) || !link)
                continue;

            std::array<wchar_t, 4096> buffer {};
            if (SUCCEEDED(link->GetArguments(buffer.data(), static_cast<int>(buffer.size()))))
                result.insert(QString::fromWCharArray(buffer.data()));
            link->Release();
        }
        return result;
    }

    QList<Note> recentVisibleNotes(int maximumCount)
    {
        auto  notes          = NoteManager::instance()->noteList(-1);
        auto *catalogManager = FolderCatalogManager::instance();
        if (catalogManager && catalogManager->isAvailable()) {
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [catalogManager](const Note &note) {
                                           return note.isNull() || note.id().isEmpty()
                                               || catalogManager->catalog().isRecycled(note.storageId(), note.id());
                                       }),
                        notes.end());
        } else {
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [](const Note &note) { return note.isNull() || note.id().isEmpty(); }),
                        notes.end());
        }

        if (notes.size() > maximumCount)
            notes.resize(maximumCount);
        return notes;
    }

} // namespace

WindowsTaskbarIntegration::WindowsTaskbarIntegration(QObject *parent) : QObject(parent), rebuildTimer_(new QTimer(this))
{
    const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(initializeResult)) {
        comInitialized_ = true;
    } else if (initializeResult != RPC_E_CHANGED_MODE) {
        qCWarning(logWindowsTaskbar) << "Failed to initialize COM for Jump Lists:" << hresultText(initializeResult);
        return;
    }

    qApp->installEventFilter(this);

    rebuildTimer_->setSingleShot(true);
    rebuildTimer_->setInterval(250);
    connect(rebuildTimer_, &QTimer::timeout, this, [this]() { rebuildJumpList(); });

    auto *manager = NoteManager::instance();
    auto *index   = manager->notesIndex();
    connect(index, &NotesIndex::storageNotesChanged, this, [this]() { scheduleRebuild(); });
    connect(index, &NotesIndex::storageStateChanged, this, [this]() { scheduleRebuild(); });
    connect(manager, &NoteManager::storageAdded, this, [this]() { scheduleRebuild(); });
    connect(manager, &NoteManager::storageRemoved, this, [this]() { scheduleRebuild(); });
    connect(manager, &NoteManager::storageOrderChanged, this, [this]() { scheduleRebuild(); });
    connect(FolderCatalogManager::instance(), &FolderCatalogManager::catalogChanged, this,
            [this]() { scheduleRebuild(); });

    QTimer::singleShot(0, this, [this]() { rebuildJumpList(); });
}

WindowsTaskbarIntegration::~WindowsTaskbarIntegration()
{
    if (qApp)
        qApp->removeEventFilter(this);
    if (comInitialized_)
        CoUninitialize();
}

bool WindowsTaskbarIntegration::eventFilter(QObject *watched, QEvent *event)
{
    if (!event || !noteWindow(watched))
        return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::Show) {
        configureNoteWindow(watched);
    } else if (event->type() == QEvent::PlatformSurface) {
        const auto *surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);
        if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            clearNoteWindowProperties(watched);
    }

    return QObject::eventFilter(watched, event);
}

void WindowsTaskbarIntegration::configureNoteWindow(QObject *object)
{
    auto *window = noteWindow(object);
    if (!window)
        return;

    const QString appId = currentAppUserModelId();
    if (appId.isEmpty()) {
        qCWarning(logWindowsTaskbar) << "Could not determine AppUserModelID for note window";
        return;
    }

    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;

    IPropertyStore *store = nullptr;
    HRESULT         hr    = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store));
    if (FAILED(hr)) {
        qCWarning(logWindowsTaskbar) << "Failed to get note window property store:" << hresultText(hr);
        return;
    }

    const QString relaunchCommand = quoteWindowsArgument(nativeLaunchExecutable()) + QStringLiteral(" -n");
    const QString displayName     = QCoreApplication::applicationName();

    // Relaunch properties must be set before the explicit window AppUserModelID.
    hr = setStringProperty(store, PKEY_AppUserModel_RelaunchCommand, relaunchCommand);
    if (SUCCEEDED(hr))
        hr = setStringProperty(store, PKEY_AppUserModel_RelaunchDisplayNameResource, displayName);
    if (SUCCEEDED(hr))
        hr = setStringProperty(store, PKEY_AppUserModel_ID, appId);
    if (SUCCEEDED(hr))
        hr = store->Commit();

    if (FAILED(hr))
        qCWarning(logWindowsTaskbar) << "Failed to configure note window relaunch properties:" << hresultText(hr);

    store->Release();
}

void WindowsTaskbarIntegration::clearNoteWindowProperties(QObject *object)
{
    auto *window = noteWindow(object);
    if (!window)
        return;

    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;

    IPropertyStore *store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))))
        return;

    PROPVARIANT empty;
    PropVariantInit(&empty);
    store->SetValue(PKEY_AppUserModel_RelaunchCommand, empty);
    store->SetValue(PKEY_AppUserModel_RelaunchDisplayNameResource, empty);
    store->SetValue(PKEY_AppUserModel_RelaunchIconResource, empty);
    store->SetValue(PKEY_AppUserModel_ID, empty);
    store->Release();
}

bool WindowsTaskbarIntegration::hasPackageIdentity()
{
    UINT32 length = 0;
    return GetCurrentPackageFamilyName(&length, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}

void WindowsTaskbarIntegration::scheduleRebuild()
{
    if (!rebuildTimer_->isActive())
        rebuildTimer_->start();
}

void WindowsTaskbarIntegration::rebuildJumpList()
{
    ICustomDestinationList *destinationList = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&destinationList));
    if (FAILED(hr)) {
        qCWarning(logWindowsTaskbar) << "Failed to create Jump List:" << hresultText(hr);
        return;
    }

    if (!hasPackageIdentity()) {
        hr = destinationList->SetAppID(UnpackagedAppId);
        if (FAILED(hr)) {
            qCWarning(logWindowsTaskbar) << "Failed to set Jump List AppUserModelID:" << hresultText(hr);
            destinationList->Release();
            return;
        }
    }

    UINT          maximumSlots = 0;
    IObjectArray *removed      = nullptr;
    hr                         = destinationList->BeginList(&maximumSlots, IID_PPV_ARGS(&removed));
    if (FAILED(hr)) {
        qCWarning(logWindowsTaskbar) << "Failed to begin Jump List update:" << hresultText(hr);
        destinationList->Release();
        return;
    }

    const QSet<QString> removedItems = removedArguments(removed);
    if (removed)
        removed->Release();

    IObjectCollection *tasks = nullptr;
    if (SUCCEEDED(createCollection(&tasks))) {
        int taskCount = 0;

        const QString newNoteIcon = exportedJumpListIcon(QStringLiteral(":/icons/new"), QStringLiteral("new-note"));
        const QString noteManagerIcon
            = exportedJumpListIcon(QStringLiteral(":/icons/manager"), QStringLiteral("note-manager"));

        const QStringList newNoteArguments { QStringLiteral("-n") };
        if (!removedItems.contains(commandLineArguments(newNoteArguments))
            && addShellLink(tasks, newNoteArguments,
                            QCoreApplication::translate("WindowsTaskbarIntegration", "New note"), {}, newNoteIcon)) {
            ++taskCount;
        }

        const QStringList noteManagerArguments { QStringLiteral("--note-manager") };
        if (!removedItems.contains(commandLineArguments(noteManagerArguments))
            && addShellLink(tasks, noteManagerArguments,
                            QCoreApplication::translate("WindowsTaskbarIntegration", "Note manager"), {},
                            noteManagerIcon)) {
            ++taskCount;
        }

        if (taskCount > 0) {
            IObjectArray *taskArray = nullptr;
            if (SUCCEEDED(tasks->QueryInterface(IID_PPV_ARGS(&taskArray)))) {
                const HRESULT taskResult = destinationList->AddUserTasks(taskArray);
                if (FAILED(taskResult))
                    qCWarning(logWindowsTaskbar) << "Failed to add Jump List tasks:" << hresultText(taskResult);
                taskArray->Release();
            }
        }
        tasks->Release();
    }

    const int  slotsForNotes = qMax(0, qMin(MaximumRecentNotes, static_cast<int>(maximumSlots)));
    const auto notes         = recentVisibleNotes(slotsForNotes);

    if (!notes.isEmpty()) {
        IObjectCollection *destinations = nullptr;
        if (SUCCEEDED(createCollection(&destinations))) {
            int added = 0;
            for (const auto &note : notes) {
                const QStringList arguments { QStringLiteral("--open-note"), note.storageId(), note.id() };
                const QString     argumentLine = commandLineArguments(arguments);
                if (removedItems.contains(argumentLine))
                    continue;

                QString title = note.displayTitle().trimmed();
                if (title.isEmpty())
                    title = QCoreApplication::translate("WindowsTaskbarIntegration", "Untitled note");
                if (addShellLink(destinations, arguments, title))
                    ++added;
            }

            if (added > 0) {
                IObjectArray *destinationArray = nullptr;
                if (SUCCEEDED(destinations->QueryInterface(IID_PPV_ARGS(&destinationArray)))) {
                    const QString categoryTitle
                        = QCoreApplication::translate("WindowsTaskbarIntegration", "Recent notes");
                    const std::wstring category  = categoryTitle.toStdWString();
                    const HRESULT categoryResult = destinationList->AppendCategory(category.c_str(), destinationArray);
                    if (FAILED(categoryResult))
                        qCWarning(logWindowsTaskbar)
                            << "Failed to add recent notes to Jump List:" << hresultText(categoryResult);
                    destinationArray->Release();
                }
            }
            destinations->Release();
        }
    }

    hr = destinationList->CommitList();
    if (FAILED(hr))
        qCWarning(logWindowsTaskbar) << "Failed to commit Jump List:" << hresultText(hr);

    destinationList->Release();
}

} // namespace AnyKeep
