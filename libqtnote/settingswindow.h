#ifndef QTNOTE_SETTINGSWINDOW_H
#define QTNOTE_SETTINGSWINDOW_H

#include "qtnote_export.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

class QQmlApplicationEngine;
class QQuickWindow;

namespace QtNote {

class SettingsController;

class QTNOTE_EXPORT SettingsWindow final : public QObject {
    Q_OBJECT

public:
    explicit SettingsWindow(SettingsController *controller, const QUrl &component, const QString &title,
                            QObject *parent = nullptr);
    ~SettingsWindow() override;

    bool isReady() const;
    void show();

signals:
    void applied();
    void closed();

private:
    QQmlApplicationEngine *engine_ { nullptr };
    SettingsController    *controller_ { nullptr };
    QPointer<QQuickWindow> window_;
    bool                   shown_ { false };
};

} // namespace QtNote

#endif // QTNOTE_SETTINGSWINDOW_H
