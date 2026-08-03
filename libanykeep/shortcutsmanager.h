#ifndef SHORTCUTSMANAGER_H
#define SHORTCUTSMANAGER_H

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QStringList>

class QAction;

namespace AnyKeep {

class GlobalShortcutsInterface;

class ShortcutsManager : public QObject {
    Q_OBJECT

public:
    class ShortcutInfo {
    public:
        QString      option;
        QString      name;
        QKeySequence key;
    };

    inline static constexpr const char *SKNoteFromSelection = "note-from-selection";

    explicit ShortcutsManager(GlobalShortcutsInterface *gs, QObject *parent = 0);
    // const QMap<QString, QString> &optionsMap() const;
    // QAction *shortcut(const QLatin1String &option);
    QKeySequence        key(const QString &option) const;
    bool                setKey(const QString &option, const QKeySequence &seq);
    QList<ShortcutInfo> all() const;
    QString             friendlyName(const QString &option) const;
    QString             lastError() const;
    QStringList         globalShortcutIds() const;

    bool registerGlobal(const char *option, QAction *action);
    void setShortcutEnable(const QString &option, bool enabled = true);

signals:
    void shortcutChanged(const QString &opt);

public slots:
    void triggerGlobal(const QString &option);

private:
    GlobalShortcutsInterface *gs;
    QStringList               globals;
    QHash<QString, QAction *> globalActions;
};

} // namespace AnyKeep

#endif // SHORTCUTSMANAGER_H
