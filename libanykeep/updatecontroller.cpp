#include "updatecontroller.h"

#include "anykeep_config.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
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

#ifdef Q_OS_WIN
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
    constexpr int  UpdateSchemaVersion        = 1;
    constexpr int  LauncherProtocolVersion    = 1;
    constexpr int  AutomaticCheckDelayMs      = 20 * 1000;
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

    QString quotePowerShellLiteral(QString value)
    {
        value.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QLatin1Char('\'') + value + QLatin1Char('\'');
    }

#ifdef Q_OS_WIN
    QString windowsPowerShellExecutable()
    {
        QString executable = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
        if (!executable.isEmpty())
            return executable;

        // The launcher and Qt Creator may use a deliberately reduced PATH.
        // Windows PowerShell ships in the system directory on supported
        // desktop Windows versions, so do not make update extraction depend
        // on that inherited PATH.
        wchar_t    systemDirectory[MAX_PATH] { };
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return { };
        const QString candidate = QDir(QString::fromWCharArray(systemDirectory, int(length)))
                                      .filePath(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe"));
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
    supported_      = !managedByStore_ && !installRoot_.isEmpty() && !manifestUrlString().isEmpty();

    QSettings settings;
    settings.beginGroup(QLatin1String(UpdateSettingsGroup));
    automaticChecksEnabled_ = settings.value(QStringLiteral("enabled"), true).toBool();

    network_        = new QNetworkAccessManager(this);
    automaticTimer_ = new QTimer(this);
    automaticTimer_->setSingleShot(true);
    automaticTimer_->setInterval(AutomaticCheckDelayMs);
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
        return managedByStore_ ? tr("Updates are managed by Microsoft Store") : tr("Automatic updates are unavailable");
    case Idle:
        return tr("Up to date");
    case Checking:
        return tr("Checking for updates…");
    case Downloading:
        return tr("Downloading AnyKeep %1…").arg(availableVersion_);
    case Preparing:
        return tr("Preparing AnyKeep %1…").arg(availableVersion_);
    case Ready:
        return tr("AnyKeep %1 is ready to install").arg(availableVersion_);
    case Applying:
        return tr("Restarting into AnyKeep %1…").arg(availableVersion_);
    case Failed:
        return errorString_.isEmpty() ? tr("Update failed") : errorString_;
    }
    return { };
}

void UpdateController::startAutomaticChecks()
{
#ifdef Q_OS_WIN
    if (!supported_ || !automaticChecksEnabled_ || state_ == Applying || state_ == Ready
        || automaticTimer_->isActive()) {
        return;
    }
    automaticTimer_->setInterval(AutomaticCheckDelayMs);
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

    QUrl manifestUrl(manifestUrlString());
    if (!isAllowedUpdateUrl(manifestUrl)) {
        setState(Failed, tr("The update manifest URL is invalid"));
        return;
    }
    QUrlQuery query(manifestUrl);
    query.addQueryItem(QStringLiteral("anykeep-check"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    manifestUrl.setQuery(query);

    automaticRequest_ = automatic;
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
    if (root.value(QStringLiteral("schema")).toInt() != UpdateSchemaVersion) {
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
    const QString format  = package.value(QStringLiteral("format")).toString(QStringLiteral("zip"));
    if (urlText.isEmpty() || hash.size() != 64 || size <= 0 || format != QLatin1String("zip")) {
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
    downloadFile_ = new QFile(temporaryArchivePath(), this);
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
        QFile::remove(temporaryArchivePath());
        if (state_ != Failed)
            setState(Failed, tr("Could not download the update: %1").arg(networkErrorText));
        return;
    }

    QString verificationError;
    if (!verifyDownloadedArchive(&verificationError)) {
        QFile::remove(temporaryArchivePath());
        setState(Failed, verificationError);
        return;
    }
    QFile::remove(archivePath());
    if (!QFile::rename(temporaryArchivePath(), archivePath())) {
        setState(Failed, tr("Could not finalize the downloaded update"));
        return;
    }
    beginExtraction();
#endif
}

bool UpdateController::verifyDownloadedArchive(QString *error) const
{
#ifdef Q_OS_WIN
    QFile file(temporaryArchivePath());
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

void UpdateController::beginExtraction()
{
#ifdef Q_OS_WIN
    QDir temp(temporaryVersionDirectory());
    if (temp.exists())
        temp.removeRecursively();
    if (!QDir().mkpath(temporaryVersionDirectory())) {
        setState(Failed, tr("Could not create the temporary update directory"));
        return;
    }

    const QString powerShell = windowsPowerShellExecutable();
    if (powerShell.isEmpty()) {
        setState(Failed, tr("Could not locate Windows PowerShell to unpack the update"));
        return;
    }

    const QString archive     = quotePowerShellLiteral(QDir::toNativeSeparators(archivePath()));
    const QString destination = quotePowerShellLiteral(QDir::toNativeSeparators(temporaryVersionDirectory()));
    const QString command
        = QStringLiteral("$ErrorActionPreference='Stop';"
                         "Add-Type -AssemblyName System.IO.Compression.FileSystem;"
                         "$archive=%1;$destination=%2;"
                         "$root=[IO.Path]::GetFullPath($destination+[IO.Path]::DirectorySeparatorChar);"
                         "$zip=[IO.Compression.ZipFile]::OpenRead($archive);"
                         "try { foreach($entry in $zip.Entries) {"
                         "if([string]::IsNullOrEmpty($entry.FullName)){continue};"
                         "$target=[IO.Path]::GetFullPath([IO.Path]::Combine($destination,$entry.FullName));"
                         "if(-not $target.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){"
                         "throw 'Unsafe archive entry: '+$entry.FullName}"
                         "} } finally { $zip.Dispose() };"
                         "[IO.Compression.ZipFile]::ExtractToDirectory($archive,$destination)")
              .arg(archive, destination);

    extractProcess_ = new QProcess(this);
    connect(extractProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) { handleExtractionFinished(code, int(status)); });
    connect(extractProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart)
            return;
        if (extractProcess_) {
            qCWarning(logUpdates) << "PowerShell update extraction failed to start:" << extractProcess_->errorString();
            extractProcess_->deleteLater();
            extractProcess_ = nullptr;
        }
        setState(Failed, tr("Could not start Windows PowerShell to unpack the update"));
    });
    extractProcess_->start(powerShell,
                           { QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                             QStringLiteral("-Command"), command });
    setState(Preparing);
#endif
}

void UpdateController::handleExtractionFinished(int exitCode, int exitStatus)
{
#ifdef Q_OS_WIN
    QByteArray extractionError;
    if (extractProcess_) {
        extractionError = extractProcess_->readAllStandardError().trimmed();
        extractProcess_->deleteLater();
        extractProcess_ = nullptr;
    }
    if (exitStatus != int(QProcess::NormalExit) || exitCode != 0) {
        qCWarning(logUpdates).noquote() << "PowerShell update extraction failed:" << extractionError;
        setState(Failed, tr("Could not unpack the downloaded update"));
        return;
    }

    QString validationError;
    if (!validatePreparedDirectory(&validationError) || !finishPreparedDirectory(&validationError)) {
        setState(Failed, validationError);
        return;
    }

    QFile::remove(archivePath());
    downloadProgress_ = 1.0;
    emit downloadProgressChanged();
    setState(Ready);
    emit updatePrepared(availableVersion_);
#else
    Q_UNUSED(exitCode)
    Q_UNUSED(exitStatus)
#endif
}

bool UpdateController::validatePreparedDirectory(QString *error) const
{
    const QDir    dir(temporaryVersionDirectory());
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
    prepared.insert(QStringLiteral("schema"), UpdateSchemaVersion);
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
    if (!supported_)
        return;
    QFile file(preparedStatePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject   object   = document.object();
    const QString       version  = object.value(QStringLiteral("version")).toString();
    if (!safeVersionName(version) || !isVersionNewer(version))
        return;
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
    if (!supported_)
        return;

    QFile file(rollbackStatePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QFile::remove(rollbackStatePath());

    const QJsonObject object  = document.object();
    const QString     version = object.value(QStringLiteral("version")).toString();
    if (object.value(QStringLiteral("schema")).toInt() != UpdateSchemaVersion || !safeVersionName(version)) {
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
    if (extractProcess_) {
        extractProcess_->kill();
        extractProcess_->deleteLater();
        extractProcess_ = nullptr;
    }
#endif
}

void UpdateController::resetTransientFiles()
{
    QDir temporary(temporaryVersionDirectory());
    if (temporary.exists())
        temporary.removeRecursively();
    QFile::remove(temporaryArchivePath());
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
        return { };
    QDir root = versionsDir;
    if (!root.cdUp())
        return { };
    if (!QFileInfo::exists(root.filePath(QStringLiteral("AnyKeepLauncher.exe")))
        || !QFileInfo::exists(root.filePath(QStringLiteral("current.version")))) {
        return { };
    }
    return root.absolutePath();
#endif // ANYKEEP_DEVEL
#else
    return { };
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
    return { };
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

QString UpdateController::archivePath() const
{
    return QDir(stagingDirectory()).filePath(QStringLiteral("package.zip"));
}
QString UpdateController::temporaryArchivePath() const { return archivePath() + QStringLiteral(".part"); }
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
