#ifndef UPDATECONTROLLER_H
#define UPDATECONTROLLER_H

#include <QObject>
#include <QStringList>

class QByteArray;
class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QProcess;
class QTimer;

namespace AnyKeep {

class UpdateController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool managedByStore READ managedByStore CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool updateReady READ updateReady NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY updateChanged)
    Q_PROPERTY(QString releaseNotesUrl READ releaseNotesUrl NOTIFY updateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    Q_PROPERTY(qreal downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)

public:
    enum State { Unsupported, Idle, Checking, Downloading, Preparing, Ready, Applying, Failed };
    Q_ENUM(State)

    explicit UpdateController(QObject *parent = nullptr);
    ~UpdateController() override;

    State   state() const { return state_; }
    bool    supported() const { return supported_; }
    bool    managedByStore() const { return managedByStore_; }
    bool    busy() const;
    bool    updateReady() const { return state_ == Ready; }
    QString currentVersion() const;
    QString availableVersion() const { return availableVersion_; }
    QString releaseNotesUrl() const { return releaseNotesUrl_; }
    QString statusText() const;
    QString errorString() const { return errorString_; }
    qreal   downloadProgress() const { return downloadProgress_; }

    void startAutomaticChecks();
    void confirmStartupProbe(const QStringList &arguments);
    bool launchPreparedUpdater(qint64 waitPid, QString *error = nullptr);

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void applyUpdate();

signals:
    void stateChanged();
    void updateChanged();
    void downloadProgressChanged();
    void updatePrepared(const QString &version);
    void applyRequested();

private:
    void setState(State state, const QString &error = {});
    void checkForUpdate(bool automatic);
    void handleManifestReply();
    bool parseManifest(const QByteArray &data, QString *error);
    void beginDownload();
    void handleDownloadReadyRead();
    void handleDownloadFinished();
    bool verifyDownloadedArchive(QString *error) const;
    void beginExtraction();
    void handleExtractionFinished(int exitCode, int exitStatus);
    bool validatePreparedDirectory(QString *error) const;
    bool finishPreparedDirectory(QString *error);
    bool savePreparedState(QString *error);
    void restorePreparedUpdate();
    void clearDownloadObjects();
    void resetTransientFiles();

    QString detectInstallRoot() const;
    QString manifestUrlString() const;
    QString stagingDirectory() const;
    QString preparedStatePath() const;
    QString finalVersionDirectory() const;
    QString archivePath() const;
    QString temporaryArchivePath() const;
    QString temporaryVersionDirectory() const;
    bool    isVersionNewer(const QString &candidate) const;

    State   state_ { Unsupported };
    bool    supported_ { false };
    bool    managedByStore_ { false };
    bool    automaticRequest_ { false };
    QString installRoot_;
    QString availableVersion_;
    QString packageUrl_;
    QString expectedSha256_;
    qint64  expectedSize_ { -1 };
    QString releaseNotesUrl_;
    QString errorString_;
    qreal   downloadProgress_ { 0.0 };

#ifdef Q_OS_WIN
    QNetworkAccessManager *network_ { nullptr };
    QNetworkReply         *reply_ { nullptr };
    QFile                 *downloadFile_ { nullptr };
    QProcess              *extractProcess_ { nullptr };
    QTimer                *automaticTimer_ { nullptr };
#endif
};

} // namespace AnyKeep

#endif // UPDATECONTROLLER_H
