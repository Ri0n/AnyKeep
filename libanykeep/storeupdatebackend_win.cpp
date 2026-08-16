#include "storeupdatebackend_win.h"

#ifdef Q_OS_WIN

#include <QDebug>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QWindow>

#include <ShObjIdl.h>
#include <windows.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Services.Store.h>
#include <winrt/base.h>

#include <utility>
#include <vector>

namespace AnyKeep {

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Services::Store;

namespace {
    QString hstringToQString(const hstring &value) { return QString::fromWCharArray(value.c_str(), int(value.size())); }

    QString hresultMessage(const hresult_error &error)
    {
        QString message = hstringToQString(error.message()).trimmed();
        if (!message.isEmpty())
            return message;
        return QObject::tr("Microsoft Store operation failed (0x%1)")
            .arg(quint32(error.code().value), 8, 16, QLatin1Char('0'));
    }

    QString packageVersionString(const Windows::ApplicationModel::PackageVersion &version)
    {
        return QStringLiteral("%1.%2.%3.%4")
            .arg(version.Major)
            .arg(version.Minor)
            .arg(version.Build)
            .arg(version.Revision);
    }

    QString stateError(StorePackageUpdateState state)
    {
        switch (state) {
        case StorePackageUpdateState::Canceled:
            return QObject::tr("The Microsoft Store update was canceled");
        case StorePackageUpdateState::ErrorLowBattery:
            return QObject::tr("Microsoft Store postponed the update because the battery is too low");
        case StorePackageUpdateState::ErrorWiFiRecommended:
            return QObject::tr("Microsoft Store recommends Wi-Fi before downloading this update");
        case StorePackageUpdateState::ErrorWiFiRequired:
            return QObject::tr("Microsoft Store requires Wi-Fi to download this update");
        case StorePackageUpdateState::OtherError:
            return QObject::tr("Microsoft Store could not complete the update");
        default:
            return QObject::tr("Microsoft Store returned update state %1").arg(int(state));
        }
    }

    HWND storeDialogOwner()
    {
        if (auto *window = QGuiApplication::focusWindow()) {
            if (window->isVisible())
                return reinterpret_cast<HWND>(window->winId());
        }
        const auto windows = QGuiApplication::topLevelWindows();
        for (auto *window : windows) {
            if (window && window->isVisible())
                return reinterpret_cast<HWND>(window->winId());
        }
        // A tray-only AnyKeep session may have no visible window when the user
        // clicks an update notification. A hidden top-level Qt window still
        // provides a valid HWND for StoreContext's modal permission UI.
        for (auto *window : windows) {
            if (window)
                return reinterpret_cast<HWND>(window->winId());
        }
        return nullptr;
    }

    StoreContext createStoreContext(bool needsUi)
    {
        StoreContext context = StoreContext::GetDefault();
        if (!needsUi)
            return context;

        const HWND owner = storeDialogOwner();
        if (!owner)
            throw hresult_error(E_FAIL, L"AnyKeep could not find a window for the Microsoft Store dialog");
        auto initializeWithWindow = context.as<::IInitializeWithWindow>();
        check_hresult(initializeWithWindow->Initialize(owner));
        return context;
    }

    bool isCanceled(StorePackageUpdateState state) { return state == StorePackageUpdateState::Canceled; }
} // namespace

class StoreUpdateBackend::Private {
public:
    std::vector<StorePackageUpdate> updates;
    bool                            canSilent { false };
    bool                            isDownloaded { false };

    void clear()
    {
        updates.clear();
        canSilent    = false;
        isDownloaded = false;
    }

    bool registerForUpdateRestart(QString *error = nullptr)
    {
        // Keep Restart Manager registration for the lifetime of a Store build,
        // not just while AnyKeep initiates an update itself. Microsoft Store can
        // also apply an automatic update outside our StoreContext request. Avoid
        // surprising crash/hang/reboot relaunches while still allowing package
        // update (patch) restart.
        constexpr DWORD flags         = RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT;
        const HRESULT   restartResult = RegisterApplicationRestart(nullptr, flags);
        if (SUCCEEDED(restartResult))
            return true;
        if (error) {
            *error = QObject::tr("Could not register AnyKeep to restart after a Microsoft Store update (0x%1)")
                         .arg(quint32(restartResult), 8, 16, QLatin1Char('0'));
        }
        return false;
    }
};

StoreUpdateBackend::StoreUpdateBackend(QObject *parent) : QObject(parent), d(std::make_unique<Private>())
{
    // Register early so Store-driven automatic updates, not only updates
    // initiated from AnyKeep, can restart this packaged desktop process.
    QString restartError;
    if (!d->registerForUpdateRestart(&restartError))
        qWarning().noquote() << restartError;
}

StoreUpdateBackend::~StoreUpdateBackend() = default;

bool StoreUpdateBackend::hasUpdates() const { return !d->updates.empty(); }
bool StoreUpdateBackend::canSilentlyDownload() const { return d->canSilent; }
bool StoreUpdateBackend::downloaded() const { return d->isDownloaded; }

void StoreUpdateBackend::checkForUpdates()
{
    d->clear();
    QPointer<StoreUpdateBackend> guard(this);
    try {
        StoreContext context   = createStoreContext(false);
        const auto   operation = context.GetAppAndOptionalStorePackageUpdatesAsync();
        operation.Completed([guard, context](const auto &async, AsyncStatus status) {
            bool                            available = false;
            bool                            canSilent = false;
            QString                         version;
            QString                         error;
            std::vector<StorePackageUpdate> copiedUpdates;
            try {
                if (status == AsyncStatus::Completed) {
                    const auto results = async.GetResults();
                    copiedUpdates.reserve(results.Size());
                    for (const auto &update : results)
                        copiedUpdates.push_back(update);
                    available = !copiedUpdates.empty();
                    canSilent = context.CanSilentlyDownloadStorePackageUpdates();
                    if (available)
                        version = packageVersionString(copiedUpdates.front().Package().Id().Version());
                } else if (status == AsyncStatus::Canceled) {
                    error = QObject::tr("Microsoft Store update check was canceled");
                } else {
                    error = QObject::tr("Microsoft Store update check failed");
                }
            } catch (const hresult_error &exception) {
                error = hresultMessage(exception);
            }

            if (!guard)
                return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, copiedUpdates, available, canSilent, version, error] {
                    if (!guard)
                        return;
                    if (error.isEmpty()) {
                        guard->d->updates      = copiedUpdates;
                        guard->d->canSilent    = canSilent;
                        guard->d->isDownloaded = false;
                    }
                    emit guard->checkFinished(available, version, canSilent, error);
                },
                Qt::QueuedConnection);
        });
    } catch (const hresult_error &exception) {
        emit checkFinished(false, {}, false, hresultMessage(exception));
    }
}

void StoreUpdateBackend::downloadUpdates(bool silentOnly)
{
    if (!hasUpdates()) {
        emit downloadFinished(false, false, tr("No Microsoft Store update is available"));
        return;
    }

    if (silentOnly && !d->canSilent) {
        emit downloadFinished(false, false, tr("Microsoft Store cannot download this update silently"));
        return;
    }

    QPointer<StoreUpdateBackend> guard(this);
    try {
        StoreContext context       = createStoreContext(!silentOnly);
        auto         copiedUpdates = d->updates;
        const auto   updates       = single_threaded_vector<StorePackageUpdate>(std::move(copiedUpdates));
        auto         operation     = silentOnly ? context.TrySilentDownloadStorePackageUpdatesAsync(updates)
                                                : context.RequestDownloadStorePackageUpdatesAsync(updates);
        operation.Progress([guard](const auto &, const StorePackageUpdateStatus &progress) {
            if (!guard)
                return;
            const qreal value = qBound<qreal>(0.0, progress.PackageDownloadProgress, 1.0);
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, value] {
                    if (guard)
                        emit guard->progressChanged(value);
                },
                Qt::QueuedConnection);
        });
        operation.Completed([guard](const auto &async, AsyncStatus status) {
            bool    success  = false;
            bool    canceled = false;
            QString error;
            try {
                if (status == AsyncStatus::Completed) {
                    const StorePackageUpdateResult result = async.GetResults();
                    success                               = result.OverallState() == StorePackageUpdateState::Completed;
                    canceled                              = isCanceled(result.OverallState());
                    if (!success && !canceled)
                        error = stateError(result.OverallState());
                } else if (status == AsyncStatus::Canceled) {
                    canceled = true;
                } else {
                    error = QObject::tr("Microsoft Store download failed");
                }
            } catch (const hresult_error &exception) {
                error = hresultMessage(exception);
            }

            if (!guard)
                return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, success, canceled, error] {
                    if (!guard)
                        return;
                    guard->d->isDownloaded = success;
                    emit guard->downloadFinished(success, canceled, error);
                },
                Qt::QueuedConnection);
        });
    } catch (const hresult_error &exception) {
        emit downloadFinished(false, false, hresultMessage(exception));
    }
}

bool StoreUpdateBackend::installUpdates(bool silentOnly, QString *error)
{
    if (!hasUpdates() || !d->isDownloaded) {
        if (error)
            *error = tr("The Microsoft Store update has not been downloaded yet");
        return false;
    }
    if (silentOnly && !d->canSilent) {
        if (error)
            *error = tr("Microsoft Store cannot install this update silently");
        return false;
    }

    // Refresh registration immediately before deployment as well. For an
    // application being updated this is the last point at which Restart Manager
    // must know how to relaunch the process.
    if (!d->registerForUpdateRestart(error))
        return false;

    QPointer<StoreUpdateBackend> guard(this);
    try {
        StoreContext context       = createStoreContext(!silentOnly);
        auto         copiedUpdates = d->updates;
        const auto   updates       = single_threaded_vector<StorePackageUpdate>(std::move(copiedUpdates));
        auto         operation     = silentOnly ? context.TrySilentDownloadAndInstallStorePackageUpdatesAsync(updates)
                                                : context.RequestDownloadAndInstallStorePackageUpdatesAsync(updates);
        operation.Progress([guard](const auto &, const StorePackageUpdateStatus &progress) {
            if (!guard)
                return;
            // When download+install is used this field spans both phases. Our
            // package is already downloaded, so any progress here is install progress.
            const qreal value = qBound<qreal>(0.0, progress.PackageDownloadProgress, 1.0);
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, value] {
                    if (guard)
                        emit guard->progressChanged(value);
                },
                Qt::QueuedConnection);
        });
        operation.Completed([guard](const auto &async, AsyncStatus status) {
            bool    success  = false;
            bool    canceled = false;
            QString error;
            try {
                if (status == AsyncStatus::Completed) {
                    const StorePackageUpdateResult result = async.GetResults();
                    success                               = result.OverallState() == StorePackageUpdateState::Completed;
                    canceled                              = isCanceled(result.OverallState());
                    if (!success && !canceled)
                        error = stateError(result.OverallState());
                } else if (status == AsyncStatus::Canceled) {
                    canceled = true;
                } else {
                    error = QObject::tr("Microsoft Store installation failed");
                }
            } catch (const hresult_error &exception) {
                error = hresultMessage(exception);
            }

            if (!guard)
                return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, success, canceled, error] {
                    if (!guard)
                        return;
                    // Keep update-restart registration even after a canceled or
                    // failed request: the Store may apply a later automatic update.
                    emit guard->installFinished(success, canceled, error);
                },
                Qt::QueuedConnection);
        });
    } catch (const hresult_error &exception) {
        if (error)
            *error = hresultMessage(exception);
        return false;
    }
    return true;
}

} // namespace AnyKeep

#endif // Q_OS_WIN
