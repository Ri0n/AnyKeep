#ifndef OPTIONSPLUGINS_H
#define OPTIONSPLUGINS_H

#include <QWidget>

namespace Ui {
class OptionsPlugins;
}

class QQuickWidget;

namespace QtNote {

class Main;
class PluginListModel;

class OptionsPlugins : public QWidget {
    Q_OBJECT

public:
    explicit OptionsPlugins(Main *qtnote, QWidget *parent = nullptr);
    ~OptionsPlugins();

private slots:
    void configurePlugin(const QString &id);

private:
    Ui::OptionsPlugins *ui;
    Main               *qtnote;
    PluginListModel    *pluginsModel;
    QQuickWidget       *pluginsView;
};

} // namespace QtNote

#endif // OPTIONSPLUGINS_H
