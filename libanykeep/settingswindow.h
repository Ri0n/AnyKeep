#ifndef ANYKEEP_SETTINGSWINDOW_H
#define ANYKEEP_SETTINGSWINDOW_H

#include "anykeep_export.h"

#include <QObject>
#include <QPointer>
#include <QUrl>

class QQmlApplicationEngine;
class QQuickWindow;

namespace AnyKeep {

class SettingsController;

class ANYKEEP_EXPORT SettingsWindow final : public QObject {
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

} // namespace AnyKeep

#endif // ANYKEEP_SETTINGSWINDOW_H
