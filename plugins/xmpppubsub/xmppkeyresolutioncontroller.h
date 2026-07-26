#ifndef XMPPKEYRESOLUTIONCONTROLLER_H
#define XMPPKEYRESOLUTIONCONTROLLER_H

#include "xmppdto.h"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QObject>

#include <functional>

namespace QtNote {

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
    Q_PROPERTY(QString keyStatus READ keyStatus NOTIFY keyStatusChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY summaryChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultTextChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY navigationChanged)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY navigationChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY navigationChanged)
    Q_PROPERTY(QString nextText READ nextText NOTIFY navigationChanged)
    Q_PROPERTY(bool completed READ completed NOTIFY completedChanged)

public:
    enum Page { ProblemPage, DevicesPage, KeysPage, ReviewPage, ResultPage };
    Q_ENUM(Page)

    using StatusCompletion = std::function<void(XmppStatusResult)>;
    using AuditCompletion  = std::function<void(XmppKeyAuditResult)>;
    using RekeyCompletion  = std::function<void(XmppRekeyResult)>;
    using TrustDevices     = std::function<void(const QList<QByteArray> &, StatusCompletion)>;
    using AuditKeys        = std::function<void(AuditCompletion)>;
    using RekeyStorage     = std::function<void(const QList<QByteArray> &, const QByteArray &, RekeyCompletion)>;

    explicit XmppKeyResolutionController(bool localKeyMissing, const QList<XmppDeviceInfo> &devices,
                                         const QString &deviceError, TrustDevices trustDevices, AuditKeys auditKeys,
                                         RekeyStorage rekeyStorage, QObject *parent = nullptr);

    int  currentPage() const { return currentPage_; }
    int  pageCount() const { return 5; }
    bool localKeyMissing() const { return localKeyMissing_; }

    QAbstractItemModel *devicesModel() const;
    QAbstractItemModel *keysModel() const;

    int     selectedKeyIndex() const { return selectedKeyIndex_; }
    QString deviceStatus() const { return deviceStatus_; }
    QString keyStatus() const { return keyStatus_; }
    QString summary() const { return summary_; }
    QString resultText() const { return resultText_; }
    bool    busy() const { return busy_; }
    bool    canGoBack() const;
    bool    canGoNext() const;
    bool    canCancel() const { return !busy_ && !completed_ && currentPage_ != ResultPage; }
    QString nextText() const;
    bool    completed() const { return completed_; }

    QByteArray      canonicalKey() const;
    XmppRekeyResult rekeyResult() const { return rekeyResult_; }

    Q_INVOKABLE void setDeviceSelected(int row, bool selected);
    Q_INVOKABLE void selectKey(int row);
    Q_INVOKABLE void next();
    Q_INVOKABLE void back();
    Q_INVOKABLE void cancel();

    /// Cancels the workflow even if a backend callback is outstanding.
    void abort();

signals:
    void currentPageChanged();
    void selectedKeyIndexChanged();
    void deviceStatusChanged();
    void keyStatusChanged();
    void summaryChanged();
    void resultTextChanged();
    void busyChanged();
    void navigationChanged();
    void completedChanged();
    void finished(bool accepted);

private:
    void              setCurrentPage(Page page);
    void              setBusy(bool busy);
    void              setDeviceStatus(const QString &status);
    void              setKeyStatus(const QString &status);
    void              populateKeys(const XmppKeyAuditResult &audit);
    void              updateSummary();
    void              finish(bool accepted);
    QList<QByteArray> availableKeys() const;

    XmppDeviceSelectionModel *devicesModel_ { nullptr };
    XmppStorageKeyModel      *keysModel_ { nullptr };
    XmppKeyAuditResult        audit_;
    XmppRekeyResult           rekeyResult_;
    TrustDevices              trustDevices_;
    AuditKeys                 auditKeys_;
    RekeyStorage              rekeyStorage_;
    QString                   deviceStatus_;
    QString                   keyStatus_;
    QString                   summary_;
    QString                   resultText_;
    Page                      currentPage_ { ProblemPage };
    int                       selectedKeyIndex_ { -1 };
    bool                      localKeyMissing_ { false };
    bool                      devicesComplete_ { false };
    bool                      rekeyComplete_ { false };
    bool                      busy_ { false };
    bool                      completed_ { false };
};

} // namespace QtNote

#endif // XMPPKEYRESOLUTIONCONTROLLER_H
