#ifndef QTNOTE_ANDROIDPLATFORMSERVICES_H
#define QTNOTE_ANDROIDPLATFORMSERVICES_H

#include <QObject>
#include <QUrl>

namespace QtNote {

class AndroidPlatformServices final : public QObject {
    Q_OBJECT

public:
    explicit AndroidPlatformServices(QObject *parent = nullptr);

    bool speechRecognitionAvailable() const;
    bool homeScreenShortcutAvailable() const;

    bool shareText(const QString &title, const QString &text);
    bool exportText(const QString &fileName, const QString &mimeType, const QByteArray &contents);
    bool requestSpeechRecognition(const QString &language = {});
    bool addHomeScreenShortcut(const QString &storageId, const QString &noteId, const QString &title);
    QUrl pendingLaunchUrl() const;

signals:
    void speechRecognized(const QString &text);
    void operationFailed(const QString &message);
    void exportCompleted();
};

} // namespace QtNote

#endif // QTNOTE_ANDROIDPLATFORMSERVICES_H
