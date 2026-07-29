#include "optionsplugins.h"

#include "pluginlistmodel.h"
#include "pluginmanager.h"
#include "qtnote.h"
#include "settingswindow.h"
#include "themediconimageprovider.h"
#include "ui_optionsplugins.h"

#include <QDebug>
#include <QIcon>
#include <QImage>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWidget>
#include <QUrl>

namespace QtNote {

namespace {
    constexpr auto PluginIconProviderId = "qtnote-plugin-icon";

    class PluginIconImageProvider final : public QQuickImageProvider {
    public:
        explicit PluginIconImageProvider(PluginListModel *model) :
            QQuickImageProvider(QQuickImageProvider::Image), model_(model)
        {
        }

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
        {
            if (!model_)
                return {};

            const QString pluginId = QUrl::fromPercentEncoding(id.toUtf8());
            QIcon         icon;
            for (int row = 0; row < model_->rowCount(); ++row) {
                const auto index = model_->index(row);
                if (model_->data(index, PluginListModel::PluginIdRole).toString() != pluginId)
                    continue;
                icon = model_->data(index, PluginListModel::IconRole).value<QIcon>();
                break;
            }
            if (icon.isNull())
                return {};

            QSize target = requestedSize.isValid() ? requestedSize : QSize(22, 22);
            target.setWidth(qMax(1, target.width()));
            target.setHeight(qMax(1, target.height()));
            QImage image = icon.pixmap(target).toImage();
            if (size)
                *size = image.size();
            return image;
        }

    private:
        QPointer<PluginListModel> model_;
    };

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

OptionsPlugins::OptionsPlugins(Main *qtnote, QWidget *parent) :
    QWidget(parent), ui(new Ui::OptionsPlugins), qtnote(qtnote)
{
    ui->setupUi(this);

    auto *mouseDisabler = new MouseDisabler(this);
    ui->ckLegendAuto->installEventFilter(mouseDisabler);
    ui->ckLegendEnabled->installEventFilter(mouseDisabler);
    ui->ckLegendDisabled->installEventFilter(mouseDisabler);
    ui->ckLegendAuto->setCheckState(Qt::PartiallyChecked);

    pluginsModel = new PluginListModel(qtnote->pluginManager(), this);
    pluginsView  = new QQuickWidget(this);
    pluginsView->setObjectName(QStringLiteral("animatedPluginsView"));
    pluginsView->setResizeMode(QQuickWidget::SizeRootObjectToView);
    pluginsView->setClearColor(palette().color(QPalette::Base));
    pluginsView->engine()->addImageProvider(QLatin1String(PluginIconProviderId),
                                            new PluginIconImageProvider(pluginsModel));
    installThemedIconImageProvider(pluginsView->engine());
    pluginsView->rootContext()->setContextProperty(QStringLiteral("settingsReorderModel"), pluginsModel);
    pluginsView->rootContext()->setContextProperty(QStringLiteral("settingsPluginMode"), true);
    pluginsView->rootContext()->setContextProperty(
        QStringLiteral("settingsPluginIconProviderPrefix"),
        QStringLiteral("image://%1/").arg(QLatin1String(PluginIconProviderId)));
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
    auto *manager    = qtnote->pluginManager();
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
        emit qtnote->settingsUpdated();
    });
    window->show();
}

} // namespace QtNote
