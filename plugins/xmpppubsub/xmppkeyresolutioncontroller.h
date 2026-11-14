#ifndef XMPPKEYRESOLUTIONCONTROLLER_H
#define XMPPKEYRESOLUTIONCONTROLLER_H

#include "xmppdto.h"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>
#include <utility>

namespace AnyKeep {

class XmppDeviceSelectionModel;
class XmppStorageKeyModel;

/**
 * @brief UI-neutral controller for restoring a missing or divergent XMPP storage key.
 *
 * The controller owns the wizard state and delegates protocol operations through
 * asynchronous callbacks. A single QML view can therefore host the workflow on
 * desktop and Android without linking the storage plugin to Qt Widgets.
 */
class XmppKeyResolutionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int currentPage READ currentPage NOTIFY currentPageChanged)
    Q_PROPERTY(int pageCount READ pageCount CONSTANT)
    Q_PROPERTY(bool localKeyMissing READ localKeyMissing CONSTANT)
    Q_PROPERTY(QAbstractItemModel *devicesModel READ devicesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *keysModel READ keysModel CONSTANT)
    Q_PROPERTY(int selectedKeyIndex READ selectedKeyIndex NOTIFY selectedKeyIndexChanged)
    Q_PROPERTY(QString deviceStatus READ deviceStatus NOTIFY deviceStatusChanged)
    Q_PROPERTY(QStringList onlineResources READ onlineResources NOTIFY onlineResourcesChanged)
    Q_PROPERTY(QString presenceStatus READ presenceStatus NOTIFY onlineResourcesChanged)
    Q_PROPERTY(bool waitingForDevices READ waitingForDevices NOTIFY waitingForDevicesChanged)
    Q_PROPERTY(QString keyStatus READ keyStatus NOTIFY keyStatusChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY summaryChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultTextChanged)
    Q_PROPERTY(QString syncSummary READ syncSummary NOTIFY syncProgressChanged)
    Q_PROPERTY(QStringList syncEvents READ syncEvents NOTIFY syncProgressChanged)
    Q_PROPERTY(int syncCompleted READ syncCompleted NOTIFY syncProgressChanged)
    Q_PROPERTY(int syncTotal READ syncTotal NOTIFY syncProgressChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY navigationChanged)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY navigationChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY navigationChanged)
    Q_PROPERTY(bool canCreateNewKey READ canCreateNewKey NOTIFY navigationChanged)
    Q_PROPERTY(bool canStartFresh READ canStartFresh NOTIFY navigationChanged)
    Q_PROPERTY(bool freshStart READ freshStart NOTIFY navigationChanged)
    Q_PROPERTY(QString nextText READ nextText NOTIFY navigationChanged)
    Q_PROPERTY(bool completed READ completed NOTIFY completedChanged)

public:
    enum Page { ProblemPage, DevicesPage, KeysPage, ReviewPage, ResultPage };
    Q_ENUM(Page)

    using StatusCompletion = std::function<void(XmppStatusResult)>;
    using AuditCompletion  = std::function<void(XmppKeyAuditResult)>;
    using RekeyCompletion  = std::function<void(XmppRekeyResult)>;
    using KeyCompletion    = std::function<void(XmppStatusResult, QByteArray, QByteArray)>;
    using TrustDevices     = std::function<void(const QList<QByteArray> &, StatusCompletion)>;
    using DiscoverResources = std::function<void(std::function<void(QStringList, QString)>)>;
    using RemoveDevice     = std::function<void(quint32, StatusCompletion)>;
    using AuditKeys        = std::function<void(AuditCompletion)>;
    using AuditNewResources = std::function<void(QStringList, AuditCompletion)>;
    using RekeyStorage     = std::function<void(const QList<QByteArray> &, const QByteArray &, RekeyCompletion)>;
    using CreateStorageKey = std::function<void(KeyCompletion)>;
    using InstallKey       = std::function<void(QByteArray, bool, StatusCompletion)>;

    explicit XmppKeyResolutionController(bool localKeyMissing, const QList<XmppDeviceInfo> &devices,
                                         const QString &deviceError, TrustDevices trustDevices,
                                         RemoveDevice removeDevice, AuditKeys auditKeys, RekeyStorage rekeyStorage,
                                         int operationTimeoutMs = 45000, CreateStorageKey createStorageKey = {},
                                         QObject *parent = nullptr, DiscoverResources discoverResources = {},
                                         int presencePollIntervalMs = 15000);

    int  currentPage() const { return currentPage_; }
    int  pageCount() const { return 5; }
    bool localKeyMissing() const { return localKeyMissing_; }

    QAbstractItemModel *devicesModel() const;
    QAbstractItemModel *keysModel() const;

    int         selectedKeyIndex() const { return selectedKeyIndex_; }
    QString     deviceStatus() const { return deviceStatus_; }
    QStringList onlineResources() const { return onlineResources_; }
    QString     presenceStatus() const { return presenceStatus_; }
    bool        waitingForDevices() const { return waitingForDevices_; }
    QString     keyStatus() const { return keyStatus_; }
    QString     summary() const { return summary_; }
    QString     resultText() const { return resultText_; }
    QString     syncSummary() const { return syncSummary_; }
    QStringList syncEvents() const { return syncEvents_; }
    int         syncCompleted() const { return syncCompleted_; }
    int         syncTotal() const { return syncTotal_; }
    bool        busy() const { return busy_; }
    bool        canGoBack() const;
    bool        canGoNext() const;
    bool        canCancel() const { return (!busy_ || waitingForDevices_) && !completed_ && currentPage_ != ResultPage; }
    bool        canCreateNewKey() const;
    bool        canStartFresh() const;
    bool        freshStart() const { return freshStart_; }
    QString     nextText() const;
    bool        completed() const { return completed_; }

    QByteArray      canonicalKey() const;
    XmppRekeyResult rekeyResult() const { return rekeyResult_; }

    /// UI-thread sink bound to the current operation; retries/finish invalidate older sinks.
    XmppProgressCallback progressCallback();
    /// Storage supplies the durable keychain/configuration commit before acceptance.
    void setKeyInstaller(InstallKey installer) { installKey_ = std::move(installer); }
    void setNewResourceAuditor(AuditNewResources callback) { auditNewResources_ = std::move(callback); }

    Q_INVOKABLE void setDeviceSelected(int row, bool selected);
    Q_INVOKABLE void removeDevice(int row);
    Q_INVOKABLE void selectKey(int row);
    Q_INVOKABLE void retryKeySearch();
    Q_INVOKABLE void checkAgain();
    Q_INVOKABLE void createNewKey();
    Q_INVOKABLE void startFresh();
    Q_INVOKABLE void next();
    Q_INVOKABLE void back();
    Q_INVOKABLE void cancel();

    /// Cancels the workflow even if a backend callback is outstanding.
    void abort();

signals:
    void currentPageChanged();
    void selectedKeyIndexChanged();
    void deviceStatusChanged();
    void onlineResourcesChanged();
    void waitingForDevicesChanged();
    void keyStatusChanged();
    void summaryChanged();
    void resultTextChanged();
    void syncProgressChanged();
    void busyChanged();
    void navigationChanged();
    void completedChanged();
    void finished(bool accepted);

private:
    void              setCurrentPage(Page page);
    void              setBusy(bool busy);
    void              setDeviceStatus(const QString &status);
    void              setKeyStatus(const QString &status);
    void              recordSyncProgress(const XmppSyncProgress &progress);
    void              populateKeys(const XmppKeyAuditResult &audit);
    void              setWaitingForDevices(bool waiting);
    void              refreshOnlineResources();
    void              requestKeyAudit(QStringList resources = {});
    void              updateSummary();
    void              finish(bool accepted);
    quint64           beginDeviceOperation(const QString &timeoutMessage);
    bool              deviceOperationIsCurrent(quint64 token) const;
    void              finishDeviceOperation(quint64 token);
    QList<QByteArray> availableKeys() const;

    XmppDeviceSelectionModel *devicesModel_ { nullptr };
    XmppStorageKeyModel      *keysModel_ { nullptr };
    XmppKeyAuditResult        audit_;
    XmppRekeyResult           rekeyResult_;
    TrustDevices              trustDevices_;
    RemoveDevice              removeDevice_;
    AuditKeys                 auditKeys_;
    DiscoverResources         discoverResources_;
    AuditNewResources         auditNewResources_;
    RekeyStorage              rekeyStorage_;
    CreateStorageKey          createStorageKey_;
    InstallKey                installKey_;
    QString                   deviceStatus_;
    QStringList               onlineResources_;
    QString                   presenceStatus_;
    QSet<QString>             rejectedResources_;
    QString                   keyStatus_;
    QString                   summary_;
    QString                   resultText_;
    QString                   syncSummary_;
    QStringList               syncEvents_;
    int                       syncCompleted_ { -1 };
    int                       syncTotal_ { -1 };
    quint64                   progressGeneration_ { 0 };
    Page                      currentPage_ { ProblemPage };
    int                       selectedKeyIndex_ { -1 };
    bool                      localKeyMissing_ { false };
    bool                      devicesComplete_ { false };
    bool                      waitingForDevices_ { false };
    bool                      presenceProbeInFlight_ { false };
    bool                      presenceKnown_ { false };
    quint64                   presenceGeneration_ { 0 };
    QTimer                    presenceTimer_;
    bool                      rekeyComplete_ { false };
    bool                      busy_ { false };
    bool                      completed_ { false };
    bool                      freshStart_ { false };
    bool                      keyInstalled_ { false };
    // The first-install new-key route skips remote key discovery. Counts are
    // unknown rather than zero, and Back must return to the welcome screen.
    bool    freshStartFromWelcome_ { false };
    QTimer  operationTimer_;
    QString operationTimeoutMessage_;
    quint64 operationToken_ { 0 };
    int     operationTimeoutMs_ { 45000 };
    int     presencePollIntervalMs_ { 15000 };
};

} // namespace AnyKeep

#endif // XMPPKEYRESOLUTIONCONTROLLER_H
