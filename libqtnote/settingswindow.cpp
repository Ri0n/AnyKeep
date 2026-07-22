#include "settingswindow.h"

#include "settingscontroller.h"

#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QVariant>

namespace QtNote {

SettingsWindow::SettingsWindow(SettingsController *controller, const QUrl &component, const QString &title,
                               QObject *parent) : QObject(parent), controller_(controller)
{
    if (!controller_)
        return;
    if (!controller_->parent())
        controller_->setParent(this);

    engine_ = new QQmlApplicationEngine(this);
    engine_->rootContext()->setContextProperty(QStringLiteral("qtnoteSettingsController"), controller_);
    engine_->rootContext()->setContextProperty(QStringLiteral("qtnoteSettingsComponent"), component);
    engine_->rootContext()->setContextProperty(QStringLiteral("qtnoteSettingsTitle"), title);
    engine_->load(QUrl(QStringLiteral("qrc:/qml/SettingsWindow.qml")));

    if (!engine_->rootObjects().isEmpty())
        window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().constFirst());
    if (!window_) {
        qWarning() << "Failed to create settings QML window for" << title << component;
        return;
    }

    window_->setProperty("settingsController", QVariant::fromValue(controller_));
    window_->setProperty("settingsContentSource", component);
    window_->setProperty("settingsTitle", title);
    connect(window_.data(), SIGNAL(settingsApplied()), this, SIGNAL(applied()));
    connect(window_.data(), &QQuickWindow::visibleChanged, this, [this](bool visible) {
        if (visible) {
            shown_ = true;
            return;
        }
        if (!shown_)
            return;
        emit closed();
        deleteLater();
    });
}

SettingsWindow::~SettingsWindow()
{
    if (controller_ && controller_->dirty())
        controller_->reset();
}

bool SettingsWindow::isReady() const { return !window_.isNull(); }

void SettingsWindow::show()
{
    if (!window_)
        return;
    window_->show();
    window_->raise();
    window_->requestActivate();
}

} // namespace QtNote
