#include <QAbstractTableModel>
#include <QDataStream>
#include <QDebug>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTransform>

#include "optionsplugins.h"
#include "pluginlistmodel.h"
#include "pluginmanager.h"
#include "qtnote.h"
#include "settingswindow.h"
#include "ui_optionsplugins.h"

namespace QtNote {

class ButtonDelegate : public QStyledItemDelegate {
    Q_OBJECT

    QModelIndex sunken;

public:
    explicit ButtonDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) { }

    // painting
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        if (opt.icon.isNull()) {
            return;
        }
        painter->save();
        if (opt.state & QStyle::State_Selected) {
            painter->setPen(QPen(Qt::NoPen));
            if (opt.state & QStyle::State_Active) {
                painter->setBrush(QBrush(QPalette().highlight()));
            } else {
                painter->setBrush(QBrush(QPalette().color(QPalette::Inactive, QPalette::Highlight)));
            }
            painter->drawRect(opt.rect);
        }

        QStyleOptionButton buttonOption;
        buttonOption.icon     = opt.icon;
        buttonOption.iconSize = option.decorationSize;
        buttonOption.text     = opt.text;
        buttonOption.features = QStyleOptionButton::Flat;
        buttonOption.rect     = opt.rect;
        buttonOption.state    = QStyle::State_Enabled;
        if (index == sunken) {
            buttonOption.state |= QStyle::State_Sunken;
        }
        if (option.state & QStyle::State_MouseOver) {
            buttonOption.state |= (QStyle::State_Active | QStyle::State_MouseOver);
        }

        QApplication::style()->drawControl(QStyle::CE_PushButton, &buttonOption, painter);
        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index)
    {
        Q_UNUSED(model);
        Q_UNUSED(option);

        if (!(event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)) {
            return true;
        }

        sunken = QModelIndex();

        if (event->type() == QEvent::MouseButtonPress) {
            sunken = index;
        }
        return true;
    }
};

class PluginsModel : public QAbstractTableModel {
    Q_OBJECT

    static constexpr auto MimeType = "application/qtnote.plugin.id";

    PluginListModel plugins;
    QIcon           settingIcon;

public:
    PluginsModel(PluginManager *pluginManager, QObject *parent) :
        QAbstractTableModel(parent), plugins(pluginManager, this)
    {
        QPixmap pix(":/icons/options");
        settingIcon = QIcon(pix);
        QTransform transform;
        transform.rotate(45);
        pix.transformed(transform, Qt::SmoothTransformation);

        settingIcon.addPixmap(pix.transformed(transform), QIcon::Active);
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : plugins.rowCount();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid()) {
            return 0;
        }
        return 3;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= plugins.rowCount())
            return {};

        const QModelIndex pluginIndex = plugins.index(index.row());
        if (index.column() == 0) {
            switch (role) {
            case Qt::DisplayRole:
            case Qt::DecorationRole:
            case Qt::CheckStateRole:
            case Qt::ToolTipRole:
            case Qt::FontRole:
            case Qt::ForegroundRole:
                return plugins.data(pluginIndex, role);
            }
        } else if (index.column() == 1) { // version
            if (role == Qt::DisplayRole)
                return plugins.data(pluginIndex, PluginListModel::VersionTextRole);
        } else if (index.column() == 2) { // settings button
            // options button
            if (role == Qt::DecorationRole && plugins.data(pluginIndex, PluginListModel::ConfigurableRole).toBool())
                return settingIcon;
        }
        return QVariant();
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override
    {
        if (index.column() == 0 && role == Qt::CheckStateRole) {
            if (!plugins.setData(plugins.index(index.row()), value, role))
                return false;
            emit dataChanged(this->index(index.row(), 0), this->index(index.row(), 2));
            return true;
        }
        return false;
    }

    QStringList mimeTypes() const override { return { QLatin1String(MimeType) }; }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        if (indexes.isEmpty()) {
            return nullptr;
        }

        const auto row = indexes.first().row();
        if (row < 0 || row >= plugins.rowCount()) {
            return nullptr;
        }

        auto        mimeData = new QMimeData();
        QByteArray  encodedData;
        QDataStream out(&encodedData, QIODevice::WriteOnly);
        out << plugins.pluginId(row);
        mimeData->setData(QLatin1String(MimeType), encodedData);
        return mimeData;
    }

    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override
    {
        Q_UNUSED(column);

        if (action == Qt::IgnoreAction) {
            return true;
        }
        if (action != Qt::MoveAction || !data->hasFormat(QLatin1String(MimeType))) {
            return false;
        }

        QString     pluginId;
        QByteArray  encodedData = data->data(QLatin1String(MimeType));
        QDataStream in(&encodedData, QIODevice::ReadOnly);
        in >> pluginId;

        int sourceRow = -1;
        for (int row = 0; row < plugins.rowCount(); ++row) {
            if (plugins.pluginId(row) == pluginId) {
                sourceRow = row;
                break;
            }
        }
        if (sourceRow < 0) {
            return false;
        }

        int destinationRow = row;
        if (destinationRow < 0) {
            destinationRow = parent.isValid() ? parent.row() : plugins.rowCount();
        }
        return moveRows(QModelIndex(), sourceRow, 1, QModelIndex(), destinationRow);
    }

    bool moveRows(const QModelIndex &sourceParent, int sourceRow, int count, const QModelIndex &destinationParent,
                  int destinationChild) override
    {
        if (sourceParent.isValid() || destinationParent.isValid() || count != 1 || sourceRow < 0
            || sourceRow >= plugins.rowCount() || destinationChild < 0 || destinationChild > plugins.rowCount()
            || destinationChild == sourceRow || destinationChild == sourceRow + 1) {
            return false;
        }

        beginMoveRows(sourceParent, sourceRow, sourceRow, destinationParent, destinationChild);
        const bool moved = plugins.moveRows({}, sourceRow, 1, {}, destinationChild);
        endMoveRows();
        return moved;
    }

    Qt::DropActions supportedDragActions() const override { return Qt::MoveAction; }
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid()) {
            return QAbstractTableModel::flags(index) | Qt::ItemIsDropEnabled;
        }

        auto flags = QAbstractTableModel::flags(index) | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
        if (index.column() == 0) {
            flags |= Qt::ItemIsUserTristate | Qt::ItemIsUserCheckable;
        }
        return flags;
    }

    QString pluginId(int row) const { return plugins.pluginId(row); }
};

class MouseDisabler : public QObject {
public:
    MouseDisabler(QObject *parent) : QObject(parent) { }
    bool eventFilter(QObject *obj, QEvent *event)
    {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
};

OptionsPlugins::OptionsPlugins(Main *qtnote, QWidget *parent) :
    QWidget(parent), ui(new Ui::OptionsPlugins), qtnote(qtnote)
{
    ui->setupUi(this);

    MouseDisabler *md = new MouseDisabler(this);
    ui->ckLegendAuto->installEventFilter(md);
    ui->ckLegendEnabled->installEventFilter(md);
    ui->ckLegendDisabled->installEventFilter(md);
    ui->ckLegendAuto->setCheckState(Qt::PartiallyChecked);

    pluginsModel = new PluginsModel(qtnote->pluginManager(), this);
    ui->tblPlugins->setModel(pluginsModel);
    ui->tblPlugins->setDragDropMode(QAbstractItemView::InternalMove);
    ui->tblPlugins->setDefaultDropAction(Qt::MoveAction);
    ui->tblPlugins->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tblPlugins->setSelectionMode(QAbstractItemView::SingleSelection);
    ButtonDelegate *btnsDelegate = new ButtonDelegate();
    ui->tblPlugins->setItemDelegateForColumn(2, btnsDelegate);
    ui->tblPlugins->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->tblPlugins->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tblPlugins->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(ui->tblPlugins, SIGNAL(clicked(QModelIndex)), SLOT(pluginClicked(QModelIndex)));
}

OptionsPlugins::~OptionsPlugins() { delete ui; }

void OptionsPlugins::pluginClicked(const QModelIndex &index)
{
    if (index.column() != 2)
        return;

    const QString id         = pluginsModel->pluginId(index.row());
    auto         *manager    = qtnote->pluginManager();
    auto         *controller = manager->createSettingsController(id, nullptr);
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

#include "optionsplugins.moc"
