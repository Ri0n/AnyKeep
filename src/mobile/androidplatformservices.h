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
    bool exportData(const QString &fileName, const QString &mimeType, const QByteArray &contents);
    bool requestSpeechRecognition(const QString &language = {});
    bool requestImage();
    bool requestFile();
    bool requestPhoto();
    bool addHomeScreenShortcut(const QString &storageId, const QString &noteId, const QString &title);
    QUrl pendingLaunchUrl() const;

signals:
    void speechRecognized(const QString &text);
    void imageSelected(const QByteArray &data, const QString &name, const QString &mediaType);
    void fileSelected(const QByteArray &data, const QString &name, const QString &mediaType);
    void operationFailed(const QString &message);
    void exportCompleted();
};

} // namespace QtNote

#endif // QTNOTE_ANDROIDPLATFORMSERVICES_H
