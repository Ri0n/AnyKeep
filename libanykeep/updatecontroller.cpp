#include "updatecontroller.h"

#include "anykeep_config.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QVersionNumber>

#include <string>

#ifdef Q_OS_WIN
#include "storeupdatebackend_win.h"
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>
#include <appmodel.h>
#include <windows.h>
#endif

namespace AnyKeep {

Q_LOGGING_CATEGORY(logUpdates, "anykeep.updates")

namespace {
    constexpr int UpdateManifestSchemaVersion = 2;
    constexpr int UpdateStateSchemaVersion    = 1;
    constexpr int LauncherProtocolVersion     = 1;
    constexpr int AutomaticCheckDelayMs       = 20 * 1000;
    // RegisterApplicationRestart only relaunches a process that has been alive
    // for at least 60 seconds. Give Store-managed builds a small safety margin
    // before the first automatic check can lead to a silent installation.
    constexpr int  StoreAutomaticCheckDelayMs = 65 * 1000;
    constexpr int  StartupProbeHealthyMs      = 60 * 1000;
    constexpr int  AutomaticCheckIntervalSecs = 6 * 60 * 60;
    constexpr int  AutomaticCheckIntervalMs   = AutomaticCheckIntervalSecs * 1000;
    constexpr auto UpdateSettingsGroup        = "updates";
    constexpr auto PreparedFileName           = "prepared.json";
    constexpr auto RollbackFileName           = "rollback.json";
    constexpr auto RollbackVersionKey         = "rollbackVersion";

    bool safeVersionName(const QString &version)
    {
        if (version.isEmpty() || version.size() > 80 || version == QLatin1String(".") || version == QLatin1String(".."))
            return false;
        for (const QChar ch : version) {
            const ushort unicode           = ch.unicode();
            const bool   asciiAlphaNumeric = (unicode >= '0' && unicode <= '9') || (unicode >= 'A' && unicode <= 'Z')
                || (unicode >= 'a' && unicode <= 'z');
            if (!(asciiAlphaNumeric || ch == QLatin1Char('.') || ch == QLatin1Char('-') || ch == QLatin1Char('_')
                  || ch == QLatin1Char('+'))) {
                return false;
            }
        }
        return true;
    }

#ifdef Q_OS_WIN
    QString windowsInstallerExecutable()
    {
        QString executable = QStandardPaths::findExecutable(QStringLiteral("msiexec.exe"));
        if (!executable.isEmpty())
            return executable;

        wchar_t    systemDirectory[MAX_PATH] {};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return {};
        const QString candidate
            = QDir(QString::fromWCharArray(systemDirectory, int(length))).filePath(QStringLiteral("msiexec.exe"));
        return QFileInfo(candidate).isExecutable() ? candidate : QString();
    }

    bool currentProcessHasPackageIdentity()
    {
        UINT32     length = 0;
        const LONG result = GetCurrentPackageFamilyName(&length, nullptr);
        return result == ERROR_INSUFFICIENT_BUFFER;
    }

    bool isAllowedUpdateUrl(const QUrl &url)
    {
        if (!url.isValid())
            return false;
        if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0)
            return true;

#ifdef ANYKEEP_DEVEL
        return url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0;
#else
        return false;
#endif
    }
#endif
} // namespace

UpdateController::UpdateController(QObject *parent) : QObject(parent)
{
#ifdef Q_OS_WIN
    managedByStore_ = currentProcessHasPackageIdentity();
    installRoot_    = detectInstallRoot();
    supported_      = managedByStore_ || (!installRoot_.isEmpty() && !manifestUrlString().isEmpty());

    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    automaticChecksEnabled_ = settings.value(QStringLiteral("enabled"), true).toBool();

    network_ = new QNetworkAccessManager(this);
    if (managedByStore_) {
        storeBackend_ = new StoreUpdateBackend(this);
        connect(storeBackend_, &StoreUpdateBackend::checkFinished, this, &UpdateController::handleStoreCheckFinished);
        connect(storeBackend_, &StoreUpdateBackend::progressChanged, this, [this](qreal progress) {
            if (!qFuzzyCompare(downloadProgress_ + 1.0, progress + 1.0)) {
                downloadProgress_ = progress;
                emit downloadProgressChanged();
            }
        });
        connect(storeBackend_, &StoreUpdateBackend::downloadFinished, this,
                &UpdateController::handleStoreDownloadFinished);
        connect(storeBackend_, &StoreUpdateBackend::installFinished, this,
                &UpdateController::handleStoreInstallFinished);
    }
    automaticTimer_ = new QTimer(this);
    automaticTimer_->setSingleShot(true);
    automaticTimer_->setInterval(managedByStore_ ? StoreAutomaticCheckDelayMs : AutomaticCheckDelayMs);
    connect(automaticTimer_, &QTimer::timeout, this, [this] {
        if (!automaticChecksEnabled_)
            return;
        checkForUpdate(true);
        if (automaticChecksEnabled_ && state_ != Applying && state_ != Ready) {
            automaticTimer_->setInterval(AutomaticCheckIntervalMs);
            automaticTimer_->start();
        }
    });

    state_ = supported_ ? Idle : Unsupported;
    restoreRollbackResult();
    restorePreparedUpdate();
#else
    state_ = Unsupported;
#endif
}

UpdateController::~UpdateController() { clearDownloadObjects(); }

bool UpdateController::busy() const
{
    return state_ == Checking || state_ == Downloading || state_ == Preparing || state_ == Applying;
}

QString UpdateController::currentVersion() const { return QStringLiteral(ANYKEEP_VERSION_STR); }

QString UpdateController::statusText() const
{
    switch (state_) {
    case Unsupported:
        return tr("Automatic updates are unavailable");
    case Idle:
        return tr("Up to date");
    case Checking:
        return tr("Checking for updates…");
    case Downloading:
        return managedByStore_ ? tr("Downloading AnyKeep %1 from Microsoft Store…").arg(availableVersion_)
                               : tr("Downloading AnyKeep %1…").arg(availableVersion_);
    case Preparing:
        return tr("Preparing AnyKeep %1…").arg(availableVersion_);
    case Ready:
        if (managedByStore_ && !storePackageDownloaded_)
            return tr("AnyKeep %1 is available in Microsoft Store").arg(availableVersion_);
        return tr("AnyKeep %1 is ready to install").arg(availableVersion_);
    case Applying:
        return managedByStore_ ? tr("Installing AnyKeep %1 from Microsoft Store…").arg(availableVersion_)
                               : tr("Restarting into AnyKeep %1…").arg(availableVersion_);
    case Failed:
        return errorString_.isEmpty() ? tr("Update failed") : errorString_;
    }
    return {};
}

void UpdateController::startAutomaticChecks()
{
#ifdef Q_OS_WIN
    if (!supported_ || !automaticChecksEnabled_ || state_ == Applying || state_ == Ready
        || automaticTimer_->isActive()) {
        return;
    }
    automaticTimer_->setInterval(managedByStore_ ? StoreAutomaticCheckDelayMs : AutomaticCheckDelayMs);
    automaticTimer_->start();
#endif
}

void UpdateController::setAutomaticChecksEnabled(bool enabled)
{
    if (automaticChecksEnabled_ == enabled)
        return;
    automaticChecksEnabled_ = enabled;

    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    settings.setValue(QStringLiteral("enabled"), enabled);

#ifdef Q_OS_WIN
    if (!enabled) {
        automaticTimer_->stop();
    } else {
        startAutomaticChecks();
    }
#endif
    emit automaticChecksEnabledChanged();
}

void UpdateController::confirmStartupProbe(const QStringList &arguments)
{
#ifdef Q_OS_WIN
    const int index = arguments.indexOf(QStringLiteral("--update-probe-file"));
    if (index < 0 || index + 1 >= arguments.size())
        return;

    const QString markerPath = QDir::cleanPath(arguments.at(index + 1));
    if (installRoot_.isEmpty())
        return;
    // Qt commonly returns absolute Windows paths with '/', while
    // QDir::separator() is '\\'. Normalize both paths before the prefix
    // check; otherwise a valid updater marker is rejected as outside staging.
    const QString allowedRoot
        = QDir::fromNativeSeparators(QDir::cleanPath(QDir(installRoot_).filePath(QStringLiteral("staging"))))
        + QLatin1Char('/');
    const QString candidate = QDir::fromNativeSeparators(QFileInfo(markerPath).absoluteFilePath());
    if (!candidate.startsWith(allowedRoot, Qt::CaseInsensitive)) {
        qCWarning(logUpdates) << "Rejected startup probe outside staging directory:" << candidate;
        return;
    }

    if (!startupProbePath_.isEmpty()) {
        if (candidate != startupProbePath_)
            qCWarning(logUpdates) << "Ignoring a second update startup probe:" << candidate;
        return;
    }

    startupProbePath_ = candidate;
    connect(qApp, &QCoreApplication::aboutToQuit, this, &UpdateController::writeStartupProbe, Qt::UniqueConnection);
    QTimer::singleShot(StartupProbeHealthyMs, this, &UpdateController::writeStartupProbe);
    qCInfo(logUpdates) << "Started the 60-second update startup probe:" << candidate;
#else
    Q_UNUSED(arguments)
#endif
}

void UpdateController::writeStartupProbe()
{
#ifdef Q_OS_WIN
    if (startupProbeWritten_ || startupProbePath_.isEmpty())
        return;

    QDir().mkpath(QFileInfo(startupProbePath_).absolutePath());
    QSaveFile marker(startupProbePath_);
    if (!marker.open(QIODevice::WriteOnly) || marker.write("ok\n") != 3 || !marker.commit()) {
        qCWarning(logUpdates) << "Failed to confirm updated application startup:" << startupProbePath_;
        return;
    }

    startupProbeWritten_ = true;
    QFile::remove(preparedStatePath());
    QDir completedStage(QDir(installRoot_).filePath(QStringLiteral("staging/%1").arg(currentVersion())));
    if (completedStage.exists())
        completedStage.removeRecursively();
    qCInfo(logUpdates) << "Confirmed a healthy updated application startup";
#endif
}

void UpdateController::checkNow() { checkForUpdate(false); }

void UpdateController::applyUpdate()
{
    if (state_ != Ready)
        return;
#ifdef Q_OS_WIN
    if (managedByStore_ && !storePackageDownloaded_) {
        automaticRequest_ = false;
        beginStoreDownload(false);
        return;
    }
#endif
    emit applyRequested();
}

bool UpdateController::launchPreparedUpdater(qint64 waitPid, QString *error)
{
#ifdef Q_OS_WIN
    if (state_ != Ready) {
        if (error)
            *error = tr("No prepared update is available");
        return false;
    }

    const QString versionDir = finalVersionDirectory();
    const QString updater    = QDir(versionDir).filePath(QStringLiteral("AnyKeepUpdater.exe"));
    if (!QFileInfo::exists(updater)) {
        if (error)
            *error = tr("The prepared update does not contain AnyKeepUpdater.exe");
        return false;
    }

    const QStringList args { QStringLiteral("--root"),     installRoot_,
                             QStringLiteral("--version"),  availableVersion_,
                             QStringLiteral("--wait-pid"), QString::number(waitPid),
                             QStringLiteral("--restart") };
    qint64            processId = 0;
    if (!QProcess::startDetached(updater, args, versionDir, &processId)) {
        if (error)
            *error = tr("Could not start the AnyKeep updater");
        return false;
    }
    qCInfo(logUpdates) << "Started prepared updater" << processId << "for" << availableVersion_;
    setState(Applying);
    return true;
#else
    Q_UNUSED(waitPid)
    if (error)
        *error = tr("Automatic updates are available only on Windows");
    return false;
#endif
}

bool UpdateController::installStoreUpdate(bool silentOnly, QString *error)
{
#ifdef Q_OS_WIN
    if (!managedByStore_ || !storeBackend_) {
        if (error)
            *error = tr("This build is not managed by Microsoft Store");
        return false;
    }
    if (state_ != Ready || !storePackageDownloaded_) {
        if (error)
            *error = tr("The Microsoft Store update is not ready to install");
        return false;
    }
    storeSilentInstallRequest_ = silentOnly;
    if (!storeBackend_->installUpdates(silentOnly, error)) {
        storeSilentInstallRequest_ = false;
        return false;
    }
    setState(Applying);
    return true;
#else
    Q_UNUSED(silentOnly)
    if (error)
        *error = tr("Microsoft Store updates are available only on Windows");
    return false;
#endif
}

void UpdateController::setState(State state, const QString &error)
{
    const bool stateChangedValue = state_ != state;
    state_                       = state;
    errorString_                 = error;
    if (stateChangedValue || !error.isEmpty())
        emit stateChanged();
}

void UpdateController::checkForUpdate(bool automatic)
{
#ifdef Q_OS_WIN
    if (!supported_ || busy() || state_ == Ready || (automatic && !automaticChecksEnabled_))
        return;

    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    if (automatic) {
        const QDateTime lastCheck = settings.value(QStringLiteral("lastCheckUtc")).toDateTime();
        if (lastCheck.isValid() && lastCheck.secsTo(QDateTime::currentDateTimeUtc()) < AutomaticCheckIntervalSecs)
            return;
    }

    automaticRequest_ = automatic;
    if (managedByStore_) {
        storePackageDownloaded_ = false;
        storeSilentAvailable_   = false;
        downloadProgress_       = 0.0;
        emit downloadProgressChanged();
        setState(Checking);
        storeBackend_->checkForUpdates();
        return;
    }

    QUrl manifestUrl(manifestUrlString());
    if (!isAllowedUpdateUrl(manifestUrl)) {
        setState(Failed, tr("The update manifest URL is invalid"));
        return;
    }
    QUrlQuery query(manifestUrl);
    query.addQueryItem(QStringLiteral("anykeep-check"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    manifestUrl.setQuery(query);

    QNetworkRequest request(manifestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Cache-Control", "no-cache");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("AnyKeep/%1 Windows updater").arg(currentVersion()));
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::finished, this, &UpdateController::handleManifestReply);
    setState(Checking);
#else
    Q_UNUSED(automatic)
#endif
}

void UpdateController::handleStoreCheckFinished(bool updateAvailable, const QString &version, bool canSilentlyDownload,
                                                const QString &error)
{
#ifdef Q_OS_WIN
    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    settings.setValue(QStringLiteral("lastCheckUtc"), QDateTime::currentDateTimeUtc());

    if (!error.isEmpty()) {
        setState(Failed, tr("Could not check Microsoft Store for updates: %1").arg(error));
        return;
    }
    if (!updateAvailable) {
        availableVersion_.clear();
        storePackageDownloaded_ = false;
        storeSilentAvailable_   = false;
        emit updateChanged();
        setState(Idle);
        return;
    }

    availableVersion_       = version;
    storeSilentAvailable_   = canSilentlyDownload;
    storePackageDownloaded_ = false;
    downloadProgress_       = 0.0;
    emit updateChanged();
    emit downloadProgressChanged();

    if (canSilentlyDownload) {
        beginStoreDownload(true);
        return;
    }

    setState(Ready);
    emit storeUpdatePrepared(availableVersion_, automaticRequest_);
#else
    Q_UNUSED(updateAvailable)
    Q_UNUSED(version)
    Q_UNUSED(canSilentlyDownload)
    Q_UNUSED(error)
#endif
}

void UpdateController::beginStoreDownload(bool silentOnly)
{
#ifdef Q_OS_WIN
    if (!managedByStore_ || !storeBackend_ || !storeBackend_->hasUpdates()) {
        setState(Failed, tr("No Microsoft Store update is available"));
        return;
    }
    downloadProgress_           = 0.0;
    storeSilentDownloadRequest_ = silentOnly;
    emit downloadProgressChanged();
    setState(Downloading);
    storeBackend_->downloadUpdates(silentOnly);
#else
    Q_UNUSED(silentOnly)
#endif
}

void UpdateController::handleStoreDownloadFinished(bool success, bool canceled, const QString &error)
{
#ifdef Q_OS_WIN
    const bool silentRequest    = storeSilentDownloadRequest_;
    storeSilentDownloadRequest_ = false;
    if (canceled) {
        // A silent background request can be canceled by Store policy without
        // user interaction. Keep the update visible so the user can retry it
        // through the normal Store consent UI.
        if (silentRequest) {
            storeSilentAvailable_ = false;
            setState(Ready);
            emit storeUpdatePrepared(availableVersion_, automaticRequest_);
            return;
        }
        setState(Ready);
        return;
    }
    if (!success) {
        // A silent download can become unavailable between the check and the
        // request (metered network, Store setting change, etc.). Keep the
        // update actionable so the user can retry with Store consent.
        if (silentRequest) {
            storeSilentAvailable_ = false;
            setState(Ready);
            emit storeUpdatePrepared(availableVersion_, automaticRequest_);
            return;
        }
        setState(Failed, error.isEmpty() ? tr("Microsoft Store could not download the update") : error);
        return;
    }

    storePackageDownloaded_ = true;
    downloadProgress_       = 1.0;
    emit downloadProgressChanged();
    setState(Ready);
    emit storeUpdatePrepared(availableVersion_, automaticRequest_);
#else
    Q_UNUSED(success)
    Q_UNUSED(canceled)
    Q_UNUSED(error)
#endif
}

void UpdateController::handleStoreInstallFinished(bool success, bool canceled, const QString &error)
{
#ifdef Q_OS_WIN
    // A successful Store deployment normally terminates the running desktop
    // process before this callback is reached. This path is primarily for
    // cancellation and deployment failures.
    const bool silentRequest   = storeSilentInstallRequest_;
    storeSilentInstallRequest_ = false;
    if (success) {
        qCInfo(logUpdates) << "Microsoft Store update completed without terminating the process";
        setState(Idle);
        return;
    }
    if (silentRequest) {
        // Store policy can change after the package was downloaded (for
        // example, a metered connection or automatic-app-update setting). Do
        // not strand a prepared update in Failed: fall back to an explicit
        // user-triggered Store installation with its normal consent UI.
        storeSilentAvailable_ = false;
        setState(Ready);
        emit storeUpdatePrepared(availableVersion_, automaticRequest_);
        return;
    }
    if (canceled) {
        setState(Ready);
        return;
    }
    setState(Failed, error.isEmpty() ? tr("Microsoft Store could not install the update") : error);
#else
    Q_UNUSED(success)
    Q_UNUSED(canceled)
    Q_UNUSED(error)
#endif
}

void UpdateController::handleManifestReply()
{
#ifdef Q_OS_WIN
    if (!reply_)
        return;
    const auto reply = reply_;
    reply_           = nullptr;

    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    settings.setValue(QStringLiteral("lastCheckUtc"), QDateTime::currentDateTimeUtc());

    const int        status           = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body             = reply->readAll();
    const auto       networkError     = reply->error();
    const QString    networkErrorText = reply->errorString();
    reply->deleteLater();

    if (automaticRequest_ && (status == 404 || status == 204)) {
        setState(Idle);
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        setState(Failed, tr("Could not check for updates: %1").arg(networkErrorText));
        return;
    }

    QString parseError;
    if (!parseManifest(body, &parseError)) {
        setState(Failed, parseError);
        return;
    }

    if (!isVersionNewer(availableVersion_)) {
        setState(Idle);
        return;
    }

    if (availableVersion_ == settings.value(QLatin1String(RollbackVersionKey)).toString()) {
        setState(Failed,
                 tr("AnyKeep %1 was rolled back because it did not start correctly. The previous version is running.")
                     .arg(availableVersion_));
        return;
    }

    if (QFileInfo::exists(QDir(finalVersionDirectory()).filePath(QStringLiteral("anykeep.exe")))
        && QFileInfo::exists(QDir(finalVersionDirectory()).filePath(QStringLiteral("AnyKeepUpdater.exe")))) {
        QString preparedError;
        if (!savePreparedState(&preparedError)) {
            setState(Failed, preparedError);
            return;
        }
        setState(Ready);
        emit updatePrepared(availableVersion_);
        return;
    }

    beginDownload();
#endif
}

bool UpdateController::parseManifest(const QByteArray &data, QString *error)
{
    QJsonParseError     jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = tr("The update manifest is not valid JSON: %1").arg(jsonError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toInt() != UpdateManifestSchemaVersion) {
        if (error)
            *error = tr("The update manifest schema is not supported");
        return false;
    }

    const int requiredLauncher = root.value(QStringLiteral("minimumLauncherProtocol")).toInt(1);
    if (requiredLauncher > LauncherProtocolVersion) {
        if (error)
            *error = tr("This update requires a newer AnyKeep installer");
        return false;
    }

    const QString version = root.value(QStringLiteral("version")).toString().trimmed();
    if (!safeVersionName(version)) {
        if (error)
            *error = tr("The update manifest contains an invalid version");
        return false;
    }

    QJsonObject package = root.value(QStringLiteral("package")).toObject();
    if (package.isEmpty())
        package = root.value(QStringLiteral("packages")).toObject().value(QStringLiteral("windows-x86_64")).toObject();
    const QString urlText = package.value(QStringLiteral("url")).toString();
    const QString hash    = package.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    const qint64  size    = package.value(QStringLiteral("size")).toVariant().toLongLong();
    const QString format  = package.value(QStringLiteral("format")).toString();
    if (urlText.isEmpty() || hash.size() != 64 || size <= 0 || format != QLatin1String("msi")) {
        if (error)
            *error = tr("The Windows update package description is incomplete");
        return false;
    }

    const QUrl baseUrl(manifestUrlString());
    const QUrl packageUrl = baseUrl.resolved(QUrl(urlText));
    if (!isAllowedUpdateUrl(packageUrl)) {
        if (error)
            *error = tr("The update package URL is invalid");
        return false;
    }

    availableVersion_ = version;
    packageUrl_       = packageUrl.toString();
    expectedSha256_   = hash;
    expectedSize_     = size;
    emit updateChanged();
    return true;
}

void UpdateController::beginDownload()
{
#ifdef Q_OS_WIN
    resetTransientFiles();
    if (!qFuzzyIsNull(downloadProgress_)) {
        downloadProgress_ = 0.0;
        emit downloadProgressChanged();
    }
    QDir().mkpath(stagingDirectory());
    downloadFile_ = new QFile(temporaryPackagePath(), this);
    if (!downloadFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setState(Failed, tr("Could not create the update download file"));
        clearDownloadObjects();
        return;
    }

    QNetworkRequest request { QUrl(packageUrl_) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, &UpdateController::handleDownloadReadyRead);
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        const qint64 denominator = total > 0 ? total : expectedSize_;
        const qreal  progress = denominator > 0 ? qBound<qreal>(0.0, qreal(received) / qreal(denominator), 1.0) : 0.0;
        if (!qFuzzyCompare(downloadProgress_ + 1.0, progress + 1.0)) {
            downloadProgress_ = progress;
            emit downloadProgressChanged();
        }
    });
    connect(reply_, &QNetworkReply::finished, this, &UpdateController::handleDownloadFinished);
    setState(Downloading);
#endif
}

void UpdateController::handleDownloadReadyRead()
{
#ifdef Q_OS_WIN
    if (!reply_ || !downloadFile_)
        return;
    const QByteArray chunk = reply_->readAll();
    if (downloadFile_->write(chunk) != chunk.size()) {
        reply_->abort();
        setState(Failed, tr("Could not write the downloaded update"));
        return;
    }
    if (expectedSize_ > 0 && downloadFile_->size() > expectedSize_) {
        reply_->abort();
        setState(Failed, tr("The downloaded update has an unexpected size"));
    }
#endif
}

void UpdateController::handleDownloadFinished()
{
#ifdef Q_OS_WIN
    if (!reply_)
        return;
    handleDownloadReadyRead();
    auto *reply                    = reply_;
    reply_                         = nullptr;
    const auto    networkError     = reply->error();
    const QString networkErrorText = reply->errorString();
    reply->deleteLater();

    if (downloadFile_) {
        downloadFile_->flush();
        downloadFile_->close();
        downloadFile_->deleteLater();
        downloadFile_ = nullptr;
    }

    if (networkError != QNetworkReply::NoError) {
        QFile::remove(temporaryPackagePath());
        if (state_ != Failed)
            setState(Failed, tr("Could not download the update: %1").arg(networkErrorText));
        return;
    }

    QString verificationError;
    if (!verifyDownloadedPackage(&verificationError)) {
        QFile::remove(temporaryPackagePath());
        setState(Failed, verificationError);
        return;
    }
    QFile::remove(packagePath());
    if (!QFile::rename(temporaryPackagePath(), packagePath())) {
        setState(Failed, tr("Could not finalize the downloaded update"));
        return;
    }
    beginPreparation();
#endif
}

bool UpdateController::verifyDownloadedPackage(QString *error) const
{
#ifdef Q_OS_WIN
    QFile file(temporaryPackagePath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Could not read the downloaded update");
        return false;
    }
    if (file.size() != expectedSize_) {
        if (error)
            *error = tr("The downloaded update has an unexpected size");
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (error)
            *error = tr("Could not verify the downloaded update");
        return false;
    }
    if (QString::fromLatin1(hash.result().toHex()) != expectedSha256_) {
        if (error)
            *error = tr("The downloaded update failed its SHA-256 check");
        return false;
    }
    return true;
#else
    Q_UNUSED(error)
    return false;
#endif
}

void UpdateController::beginPreparation()
{
#ifdef Q_OS_WIN
    QDir temp(temporaryVersionDirectory());
    if (temp.exists())
        temp.removeRecursively();
    QDir admin(administrativeImageDirectory());
    if (admin.exists())
        admin.removeRecursively();
    if (!QDir().mkpath(administrativeImageDirectory())) {
        setState(Failed, tr("Could not create the temporary MSI extraction directory"));
        return;
    }

    const QString installer = windowsInstallerExecutable();
    if (installer.isEmpty()) {
        setState(Failed, tr("Could not locate Windows Installer to prepare the update"));
        return;
    }

    QFile::remove(msiLogPath());
    const QStringList arguments {
        QStringLiteral("/a"),
        QDir::toNativeSeparators(packagePath()),
        QStringLiteral("/qn"),
        QStringLiteral("/norestart"),
        QStringLiteral("TARGETDIR=%1").arg(QDir::toNativeSeparators(administrativeImageDirectory())),
        QStringLiteral("/L*V"),
        QDir::toNativeSeparators(msiLogPath()),
    };

    prepareProcess_ = new QProcess(this);
    connect(prepareProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) { handlePreparationFinished(code, int(status)); });
    connect(prepareProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart)
            return;
        if (prepareProcess_) {
            qCWarning(logUpdates) << "Windows Installer update preparation failed to start:"
                                  << prepareProcess_->errorString();
            prepareProcess_->deleteLater();
            prepareProcess_ = nullptr;
        }
        setState(Failed, tr("Could not start Windows Installer to prepare the update"));
    });
    prepareProcess_->start(installer, arguments);
    setState(Preparing);
#endif
}

void UpdateController::handlePreparationFinished(int exitCode, int exitStatus)
{
#ifdef Q_OS_WIN
    QByteArray processError;
    if (prepareProcess_) {
        processError = prepareProcess_->readAllStandardError().trimmed();
        prepareProcess_->deleteLater();
        prepareProcess_ = nullptr;
    }
    if (exitStatus != int(QProcess::NormalExit) || exitCode != 0) {
        qCWarning(logUpdates).noquote() << "Windows Installer administrative extraction failed with exit code"
                                        << exitCode << ':' << processError << "Log:" << msiLogPath();
        setState(Failed, tr("Could not extract the downloaded MSI update"));
        return;
    }

    const QString extractedVersion = locateAdministrativeVersionDirectory();
    QString       validationError;
    if (extractedVersion.isEmpty()) {
        setState(Failed, tr("The MSI update does not contain the expected version payload"));
        return;
    }
    if (!validateVersionDirectory(extractedVersion, &validationError)) {
        setState(Failed, validationError);
        return;
    }

    QDir temporary(temporaryVersionDirectory());
    if (temporary.exists() && !temporary.removeRecursively()) {
        setState(Failed, tr("Could not replace an incomplete temporary update"));
        return;
    }
    if (!QDir().mkpath(QFileInfo(temporaryVersionDirectory()).absolutePath())) {
        setState(Failed, tr("Could not create the versions directory"));
        return;
    }

    const std::wstring source      = QDir::toNativeSeparators(extractedVersion).toStdWString();
    const std::wstring destination = QDir::toNativeSeparators(temporaryVersionDirectory()).toStdWString();
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        qCWarning(logUpdates) << "Could not move the MSI administrative payload into the prepared version directory:"
                              << GetLastError();
        setState(Failed, tr("Could not prepare the extracted MSI update"));
        return;
    }

    QDir(administrativeImageDirectory()).removeRecursively();
    if (!validateVersionDirectory(temporaryVersionDirectory(), &validationError)
        || !finishPreparedDirectory(&validationError)) {
        setState(Failed, validationError);
        return;
    }

    QFile::remove(packagePath());
    downloadProgress_ = 1.0;
    emit downloadProgressChanged();
    setState(Ready);
    emit updatePrepared(availableVersion_);
#else
    Q_UNUSED(exitCode)
    Q_UNUSED(exitStatus)
#endif
}

QString UpdateController::locateAdministrativeVersionDirectory() const
{
#ifdef Q_OS_WIN
    QDirIterator iterator(administrativeImageDirectory(), QDir::Dirs | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString   path = iterator.next();
        const QFileInfo info(path);
        if (info.fileName().compare(availableVersion_, Qt::CaseInsensitive) != 0)
            continue;
        const QDir parent = info.dir();
        if (parent.dirName().compare(QStringLiteral("versions"), Qt::CaseInsensitive) != 0)
            continue;
        if (validateVersionDirectory(path, nullptr))
            return path;
    }
#endif
    return {};
}

bool UpdateController::validateVersionDirectory(const QString &path, QString *error) const
{
    const QDir    dir(path);
    const QString app     = dir.filePath(QStringLiteral("anykeep.exe"));
    const QString updater = dir.filePath(QStringLiteral("AnyKeepUpdater.exe"));
    if (!QFileInfo(app).isFile() || !QFileInfo(updater).isFile()) {
        if (error)
            *error = tr("The update package does not contain the required executables");
        return false;
    }
    return true;
}

bool UpdateController::finishPreparedDirectory(QString *error)
{
    const QString destination = finalVersionDirectory();
    if (QFileInfo::exists(destination)) {
        QDir existing(destination);
        if (!existing.removeRecursively()) {
            if (error)
                *error = tr("Could not replace an incomplete prepared update");
            return false;
        }
    }
    QDir versions(QDir(installRoot_).filePath(QStringLiteral("versions")));
    if (!versions.exists() && !QDir().mkpath(versions.absolutePath())) {
        if (error)
            *error = tr("Could not create the versions directory");
        return false;
    }
    if (!versions.rename(QFileInfo(temporaryVersionDirectory()).fileName(), availableVersion_)) {
        if (error)
            *error = tr("Could not finalize the prepared update directory");
        return false;
    }

    return savePreparedState(error);
}

bool UpdateController::savePreparedState(QString *error)
{
    const QString path = preparedStatePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = tr("Could not save the prepared update state");
        return false;
    }

    QJsonObject prepared;
    prepared.insert(QStringLiteral("schema"), UpdateStateSchemaVersion);
    prepared.insert(QStringLiteral("version"), availableVersion_);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(prepared).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        if (error)
            *error = tr("Could not save the prepared update state");
        return false;
    }
    return true;
}

void UpdateController::restorePreparedUpdate()
{
#ifdef Q_OS_WIN
    if (!supported_ || managedByStore_)
        return;
    QFile file(preparedStatePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject   object   = document.object();
    const QString       version  = object.value(QStringLiteral("version")).toString();
    if (object.value(QStringLiteral("schema")).toInt() != UpdateStateSchemaVersion || !safeVersionName(version)
        || !isVersionNewer(version)) {
        return;
    }
    const QDir dir(QDir(installRoot_).filePath(QStringLiteral("versions/%1").arg(version)));
    if (!QFileInfo::exists(dir.filePath(QStringLiteral("anykeep.exe")))
        || !QFileInfo::exists(dir.filePath(QStringLiteral("AnyKeepUpdater.exe")))) {
        return;
    }
    availableVersion_ = version;
    state_            = Ready;
#endif
}

void UpdateController::restoreRollbackResult()
{
#ifdef Q_OS_WIN
    if (!supported_ || managedByStore_)
        return;

    QFile file(rollbackStatePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QFile::remove(rollbackStatePath());

    const QJsonObject object  = document.object();
    const QString     version = object.value(QStringLiteral("version")).toString();
    if (object.value(QStringLiteral("schema")).toInt() != UpdateStateSchemaVersion || !safeVersionName(version)) {
        qCWarning(logUpdates) << "Ignoring an invalid update rollback result";
        return;
    }

    QFile::remove(preparedStatePath());
    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    settings.setValue(QLatin1String(RollbackVersionKey), version);

    availableVersion_ = version;
    setState(Failed,
             tr("AnyKeep %1 was rolled back because it did not start correctly. The previous version is running.")
                 .arg(version));
    qCWarning(logUpdates) << "The update to" << version << "was rolled back";
#endif
}

void UpdateController::clearDownloadObjects()
{
#ifdef Q_OS_WIN
    if (reply_) {
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (downloadFile_) {
        downloadFile_->close();
        downloadFile_->deleteLater();
        downloadFile_ = nullptr;
    }
    if (prepareProcess_) {
        prepareProcess_->kill();
        prepareProcess_->deleteLater();
        prepareProcess_ = nullptr;
    }
#endif
}

void UpdateController::resetTransientFiles()
{
    QDir temporary(temporaryVersionDirectory());
    if (temporary.exists())
        temporary.removeRecursively();
    QDir administrative(administrativeImageDirectory());
    if (administrative.exists())
        administrative.removeRecursively();
    QFile::remove(temporaryPackagePath());
    QFile::remove(msiLogPath());
}

QString UpdateController::detectInstallRoot() const
{
#ifdef Q_OS_WIN
#ifdef ANYKEEP_DEVEL
    const QString overrideRoot = qEnvironmentVariable("ANYKEEP_UPDATE_ROOT").trimmed();
    return overrideRoot.isEmpty() ? QString() : QFileInfo(overrideRoot).absoluteFilePath();
#else
    QDir applicationDir(QCoreApplication::applicationDirPath());
    QDir versionsDir = applicationDir;
    if (!versionsDir.cdUp() || versionsDir.dirName().compare(QStringLiteral("versions"), Qt::CaseInsensitive) != 0)
        return {};
    QDir root = versionsDir;
    if (!root.cdUp())
        return {};
    if (!QFileInfo::exists(root.filePath(QStringLiteral("AnyKeepLauncher.exe")))
        || !QFileInfo::exists(root.filePath(QStringLiteral("current.version")))) {
        return {};
    }
    return root.absolutePath();
#endif // ANYKEEP_DEVEL
#else
    return {};
#endif
}

QString UpdateController::manifestUrlString() const
{
#ifdef Q_OS_WIN
#ifdef ANYKEEP_DEVEL
    const QString environment = qEnvironmentVariable("ANYKEEP_UPDATE_MANIFEST_URL").trimmed();
    if (!environment.isEmpty())
        return environment;
#endif
    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    return settings.value(QStringLiteral("manifestUrl"), QStringLiteral(ANYKEEP_UPDATE_MANIFEST_URL))
        .toString()
        .trimmed();
#else
    return {};
#endif
}

QString UpdateController::stagingDirectory() const
{
    return QDir(installRoot_).filePath(QStringLiteral("staging/%1").arg(availableVersion_));
}

QString UpdateController::preparedStatePath() const
{
    return QDir(installRoot_).filePath(QStringLiteral("staging/") + QLatin1String(PreparedFileName));
}

QString UpdateController::rollbackStatePath() const
{
    return QDir(installRoot_).filePath(QStringLiteral("staging/") + QLatin1String(RollbackFileName));
}

QString UpdateController::finalVersionDirectory() const
{
    return QDir(installRoot_).filePath(QStringLiteral("versions/%1").arg(availableVersion_));
}

QString UpdateController::packagePath() const
{
    return QDir(stagingDirectory()).filePath(QStringLiteral("package.msi"));
}
QString UpdateController::temporaryPackagePath() const { return packagePath() + QStringLiteral(".part"); }
QString UpdateController::administrativeImageDirectory() const
{
    return QDir(stagingDirectory()).filePath(QStringLiteral("admin-image"));
}
QString UpdateController::msiLogPath() const
{
    return QDir(stagingDirectory()).filePath(QStringLiteral("msiexec.log"));
}
QString UpdateController::temporaryVersionDirectory() const
{
    return QDir(installRoot_).filePath(QStringLiteral("versions/.%1.tmp").arg(availableVersion_));
}

bool UpdateController::isVersionNewer(const QString &candidate) const
{
    const QVersionNumber current  = QVersionNumber::fromString(currentVersion());
    const QVersionNumber proposed = QVersionNumber::fromString(candidate);
    return !current.isNull() && !proposed.isNull() && QVersionNumber::compare(proposed, current) > 0;
}

} // namespace AnyKeep
