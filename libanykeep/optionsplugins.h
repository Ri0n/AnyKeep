#ifndef OPTIONSPLUGINS_H
#define OPTIONSPLUGINS_H

#include <QWidget>

namespace Ui {
class OptionsPlugins;
}

class QQuickWidget;

namespace AnyKeep {

class Main;
class PluginListModel;

class OptionsPlugins : public QWidget {
    Q_OBJECT

public:
    explicit OptionsPlugins(Main *anykeep, QWidget *parent = nullptr);
    ~OptionsPlugins();

private slots:
    void configurePlugin(const QString &id);

private:
    Ui::OptionsPlugins *ui;
    Main               *anykeep;
    PluginListModel    *pluginsModel;
    QQuickWidget       *pluginsView;
};

} // namespace AnyKeep

#endif // OPTIONSPLUGINS_H
