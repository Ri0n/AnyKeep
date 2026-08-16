#ifndef STOREUPDATEBACKEND_WIN_H
#define STOREUPDATEBACKEND_WIN_H

#include <QObject>
#include <QString>

#include <memory>

namespace AnyKeep {

class StoreUpdateBackend final : public QObject {
    Q_OBJECT

public:
    explicit StoreUpdateBackend(QObject *parent = nullptr);
    ~StoreUpdateBackend() override;

    bool hasUpdates() const;
    bool canSilentlyDownload() const;
    bool downloaded() const;

    void checkForUpdates();
    void downloadUpdates(bool silentOnly);
    bool installUpdates(bool silentOnly, QString *error = nullptr);

signals:
    void checkFinished(bool updateAvailable, const QString &version, bool canSilentlyDownload, const QString &error);
    void progressChanged(qreal progress);
    void downloadFinished(bool success, bool canceled, const QString &error);
    void installFinished(bool success, bool canceled, const QString &error);

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace AnyKeep

#endif // STOREUPDATEBACKEND_WIN_H
