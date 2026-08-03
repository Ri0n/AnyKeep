#ifndef GLOBALSHORTCUTSINTERFACE_H
#define GLOBALSHORTCUTSINTERFACE_H

class QKeySequence;
class QObject;
class QString;
class QAction;

namespace AnyKeep {

class GlobalShortcutsInterface {
public:
    virtual bool    registerGlobalShortcut(const QString &id, const QKeySequence &key, QAction *action) = 0;
    virtual bool    updateGlobalShortcut(const QString &id, const QKeySequence &key)                    = 0;
    virtual void    setGlobalShortcutEnabled(const QString &id, bool enabled = true)                    = 0;
    virtual QString lastGlobalShortcutError() const { return {}; }
    // TODO unregister. add string identifier
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::GlobalShortcutsInterface, "com.rion-soft.AnyKeep.GlobalShortcutsInterface/1.1")

#endif // GLOBALSHORTCUTSINTERFACE_H
