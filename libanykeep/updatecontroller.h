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
    Q_PROPERTY(bool automaticChecksEnabled READ automaticChecksEnabled WRITE setAutomaticChecksEnabled NOTIFY
                   automaticChecksEnabledChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool updateReady READ updateReady NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY updateChanged)
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
    bool    automaticChecksEnabled() const { return automaticChecksEnabled_; }
    bool    busy() const;
    bool    updateReady() const { return state_ == Ready; }
    QString currentVersion() const;
    QString availableVersion() const { return availableVersion_; }
    QString statusText() const;
    QString errorString() const { return errorString_; }
    qreal   downloadProgress() const { return downloadProgress_; }

    void startAutomaticChecks();
    void confirmStartupProbe(const QStringList &arguments);
    void setAutomaticChecksEnabled(bool enabled);
    bool launchPreparedUpdater(qint64 waitPid, QString *error = nullptr);

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void applyUpdate();

signals:
    void stateChanged();
    void automaticChecksEnabledChanged();
    void updateChanged();
    void downloadProgressChanged();
    void updatePrepared(const QString &version);
    void applyRequested();

private:
    void    setState(State state, const QString &error = {});
    void    writeStartupProbe();
    void    checkForUpdate(bool automatic);
    void    handleManifestReply();
    bool    parseManifest(const QByteArray &data, QString *error);
    void    beginDownload();
    void    handleDownloadReadyRead();
    void    handleDownloadFinished();
    bool    verifyDownloadedPackage(QString *error) const;
    void    beginPreparation();
    void    handlePreparationFinished(int exitCode, int exitStatus);
    bool    validateVersionDirectory(const QString &path, QString *error) const;
    QString locateAdministrativeVersionDirectory() const;
    bool    finishPreparedDirectory(QString *error);
    bool    savePreparedState(QString *error);
    void    restoreRollbackResult();
    void    restorePreparedUpdate();
    void    clearDownloadObjects();
    void    resetTransientFiles();

    QString detectInstallRoot() const;
    QString manifestUrlString() const;
    QString stagingDirectory() const;
    QString preparedStatePath() const;
    QString rollbackStatePath() const;
    QString finalVersionDirectory() const;
    QString packagePath() const;
    QString temporaryPackagePath() const;
    QString administrativeImageDirectory() const;
    QString msiLogPath() const;
    QString temporaryVersionDirectory() const;
    bool    isVersionNewer(const QString &candidate) const;

    State   state_ { Unsupported };
    bool    supported_ { false };
    bool    managedByStore_ { false };
    bool    automaticChecksEnabled_ { true };
    bool    automaticRequest_ { false };
    bool    startupProbeWritten_ { false };
    QString installRoot_;
    QString startupProbePath_;
    QString availableVersion_;
    QString packageUrl_;
    QString expectedSha256_;
    qint64  expectedSize_ { -1 };
    QString errorString_;
    qreal   downloadProgress_ { 0.0 };

#ifdef Q_OS_WIN
    QNetworkAccessManager *network_ { nullptr };
    QNetworkReply         *reply_ { nullptr };
    QFile                 *downloadFile_ { nullptr };
    QProcess              *prepareProcess_ { nullptr };
    QTimer                *automaticTimer_ { nullptr };
#endif
};

} // namespace AnyKeep

#endif // UPDATECONTROLLER_H
