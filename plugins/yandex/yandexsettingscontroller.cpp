#include "yandexsettingscontroller.h"

#include <QNetworkReply>
#include <QNetworkRequest>

#include <utility>

#include "yandexapiutils.h"
#include "yandexplugin.h"

namespace AnyKeep {
namespace {

    const QUrl RecognizeUrl(QStringLiteral("https://stt.api.cloud.yandex.net/stt/v3/recognizeFileAsync"));

} // namespace

YandexSettingsController::YandexSettingsController(YandexPlugin *plugin, QObject *parent) :
    SettingsController(parent), plugin_(plugin)
{
    const auto settings = plugin_->settings();

    Field apiKeyField { QStringLiteral("apiKey"), tr("API key"),
                        tr("Create the key in Yandex AI Studio and paste its secret value here. Keep it private."),
                        Password, settings.apiKey };
    apiKeyField.placeholder = tr("Paste API key");
    addField(std::move(apiKeyField));
    addField({ QStringLiteral("deferredRecognition"), tr("Use deferred recognition"),
               tr("Uses the lower-cost deferred-general model. Results can take much longer; keep AnyKeep running "
                  "until the transcript arrives."),
               Boolean, settings.deferredRecognition });
    addField({ QStringLiteral("normalizeText"), tr("Normalize numbers and dates"),
               tr("Converts spoken numbers, dates, and times to their written numeric form."), Boolean,
               settings.normalizeText });
    addField({ QStringLiteral("literatureText"), tr("Literary text formatting"),
               tr("Lets SpeechKit add punctuation and rewrite the transcript into a more literary form."), Boolean,
               settings.literatureText });
    addField({ QStringLiteral("usage"), tr("Usage"),
               tr("Yandex bills every accepted asynchronous recognition session for at least 15 seconds. "
                  "For longer mono audio, the duration is rounded up to whole seconds. The estimate shown here "
                  "does not replace the Yandex Cloud billing report."),
               ReadOnly, plugin_->speechRecognitionUsageStats().humanSummary });

    connect(plugin_, &YandexPlugin::usageChanged, this, &YandexSettingsController::usageSummaryChanged);
    connect(this, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
                if (topLeft.row() > 0 || bottomRight.row() < 0)
                    return;
                if (checking_) {
                    clearCheckReply();
                    setChecking(false);
                }
                setKeyStatus({}, false);
            });
}

YandexSettingsController::~YandexSettingsController() { clearCheckReply(); }

bool    YandexSettingsController::checking() const { return checking_; }
QString YandexSettingsController::keyStatus() const { return keyStatus_; }
bool    YandexSettingsController::keyStatusError() const { return keyStatusError_; }
QString YandexSettingsController::usageSummary() const
{
    return plugin_ ? plugin_->speechRecognitionUsageStats().humanSummary : QString();
}

void YandexSettingsController::checkApiKey()
{
    if (checking_)
        return;

    const QString apiKey = value(QStringLiteral("apiKey")).toString().trimmed();
    if (apiKey.isEmpty()) {
        setKeyStatus(tr("Enter an API key first."), true);
        return;
    }

    setKeyStatus({}, false);
    setChecking(true);

    auto request = YandexApi::authenticatedRequest(RecognizeUrl, apiKey);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("application/json"));
    // An empty recognition request is rejected during request validation, but
    // only after authentication and authorization have succeeded. No audio is
    // submitted and no billable recognition operation is created.
    checkReply_ = plugin_->network_->post(request, QByteArrayLiteral("{}"));
    connect(checkReply_, &QNetworkReply::finished, this, [this] {
        if (!checkReply_)
            return;

        auto *reply             = checkReply_.data();
        checkReply_             = nullptr;
        const QByteArray body   = reply->readAll();
        const int        status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString    error  = YandexApi::errorMessage(reply, body);
        reply->deleteLater();
        setChecking(false);

        // SpeechKit returns HTTP 400 for the deliberately incomplete request
        // after it has accepted the API key. Any 2xx response also proves that
        // authentication succeeded, although it is not expected here.
        if (status == 400 || (status >= 200 && status < 300)) {
            setKeyStatus(tr("API key is accepted by Yandex SpeechKit."), false);
            return;
        }
        setKeyStatus(error, true);
    });
}

void YandexSettingsController::resetUsageStats()
{
    if (plugin_)
        plugin_->resetUsage();
}

bool YandexSettingsController::applyValues(const QVariantMap &values, QString *error)
{
    YandexPlugin::Settings settings;
    settings.apiKey              = values.value(QStringLiteral("apiKey")).toString().trimmed();
    settings.deferredRecognition = values.value(QStringLiteral("deferredRecognition")).toBool();
    settings.normalizeText       = values.value(QStringLiteral("normalizeText")).toBool();
    settings.literatureText      = values.value(QStringLiteral("literatureText")).toBool();
    if (settings.literatureText)
        settings.normalizeText = true;
    Q_UNUSED(error);
    plugin_->saveSettings(settings);
    return true;
}

void YandexSettingsController::setChecking(bool checking)
{
    if (checking_ == checking)
        return;
    checking_ = checking;
    emit checkingChanged();
}

void YandexSettingsController::setKeyStatus(const QString &status, bool error)
{
    if (keyStatus_ == status && keyStatusError_ == error)
        return;
    keyStatus_      = status;
    keyStatusError_ = error;
    emit keyStatusChanged();
}

void YandexSettingsController::clearCheckReply()
{
    if (!checkReply_)
        return;
    auto *reply = checkReply_.data();
    checkReply_ = nullptr;
    reply->abort();
    reply->deleteLater();
}

} // namespace AnyKeep
