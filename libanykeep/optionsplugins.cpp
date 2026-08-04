#include "optionsplugins.h"

#include "anykeep.h"
#include "pluginiconimageprovider.h"
#include "pluginlistmodel.h"
#include "pluginmanager.h"
#include "settingswindow.h"
#include "themediconimageprovider.h"
#include "ui_optionsplugins.h"

#include <QDebug>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>

namespace AnyKeep {

namespace {
    class MouseDisabler : public QObject {
    public:
        using QObject::QObject;

        bool eventFilter(QObject *object, QEvent *event) override
        {
            if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
                return true;
            return QObject::eventFilter(object, event);
        }
    };
}

OptionsPlugins::OptionsPlugins(Main *anykeep, QWidget *parent) :
    QWidget(parent), ui(new Ui::OptionsPlugins), anykeep(anykeep)
{
    ui->setupUi(this);

    auto *mouseDisabler = new MouseDisabler(this);
    ui->ckLegendAuto->installEventFilter(mouseDisabler);
    ui->ckLegendEnabled->installEventFilter(mouseDisabler);
    ui->ckLegendDisabled->installEventFilter(mouseDisabler);
    ui->ckLegendAuto->setCheckState(Qt::PartiallyChecked);

    pluginsModel = new PluginListModel(anykeep->pluginManager(), this);
    pluginsView  = new QQuickWidget(this);
    pluginsView->setObjectName(QStringLiteral("animatedPluginsView"));
    pluginsView->setResizeMode(QQuickWidget::SizeRootObjectToView);
    pluginsView->setClearColor(palette().color(QPalette::Base));
    installPluginIconImageProvider(pluginsView->engine());
    installThemedIconImageProvider(pluginsView->engine());
    pluginsView->rootContext()->setContextProperty(QStringLiteral("settingsReorderModel"), pluginsModel);
    pluginsView->rootContext()->setContextProperty(QStringLiteral("settingsPluginMode"), true);
    pluginsView->rootContext()->setContextProperty(QStringLiteral("settingsPluginIconProviderPrefix"),
                                                   QStringLiteral("image://anykeep-plugin-icon/"));
    pluginsView->setSource(QUrl(QStringLiteral("qrc:/qml/AnimatedSettingsList.qml")));

    delete ui->verticalLayout->replaceWidget(ui->tblPlugins, pluginsView);
    ui->tblPlugins->hide();

    if (pluginsView->status() != QQuickWidget::Ready) {
        qWarning() << "Failed to create the animated plugin settings list:" << pluginsView->errors();
    } else {
        connect(pluginsView->rootObject(), SIGNAL(configureRequested(QString)), this, SLOT(configurePlugin(QString)));
    }
}

OptionsPlugins::~OptionsPlugins() { delete ui; }

void OptionsPlugins::configurePlugin(const QString &id)
{
    auto *manager    = anykeep->pluginManager();
    auto *controller = manager->createSettingsController(id, nullptr);
    if (!controller)
        return;

    const auto &metadata  = manager->metadata(id);
    auto        component = manager->settingsComponent(id);
    if (component.isEmpty())
        component = QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml"));
    auto *window
        = new SettingsWindow(controller, component, metadata.name + QStringLiteral(": ") + tr("Settings"), this);
    connect(window, &SettingsWindow::applied, this, [this, id]() {
        qDebug() << "Plugin settings applied:" << id << "emitting settingsUpdated";
        emit anykeep->settingsUpdated();
    });
    window->show();
}

} // namespace AnyKeep
