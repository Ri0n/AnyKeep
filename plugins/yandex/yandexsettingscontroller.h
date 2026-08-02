#ifndef YANDEXSETTINGSCONTROLLER_H
#define YANDEXSETTINGSCONTROLLER_H

#include <QPointer>

#include "settingscontroller.h"

class QNetworkReply;

namespace QtNote {

class YandexPlugin;

class YandexSettingsController final : public SettingsController {
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(QString keyStatus READ keyStatus NOTIFY keyStatusChanged)
    Q_PROPERTY(bool keyStatusError READ keyStatusError NOTIFY keyStatusChanged)
    Q_PROPERTY(QString usageSummary READ usageSummary NOTIFY usageSummaryChanged)

public:
    explicit YandexSettingsController(YandexPlugin *plugin, QObject *parent = nullptr);
    ~YandexSettingsController() override;

    bool    checking() const;
    QString keyStatus() const;
    bool    keyStatusError() const;
    QString usageSummary() const;

    Q_INVOKABLE void checkApiKey();
    Q_INVOKABLE void resetUsageStats();

signals:
    void checkingChanged();
    void keyStatusChanged();
    void usageSummaryChanged();

protected:
    bool applyValues(const QVariantMap &values, QString *error) override;

private:
    void setChecking(bool checking);
    void setKeyStatus(const QString &status, bool error);
    void clearCheckReply();

    YandexPlugin           *plugin_ = nullptr;
    QPointer<QNetworkReply> checkReply_;
    QString                 keyStatus_;
    bool                    checking_ { false };
    bool                    keyStatusError_ { false };
};

} // namespace QtNote

#endif // YANDEXSETTINGSCONTROLLER_H
