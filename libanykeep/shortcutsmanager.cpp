#include <QAction>
#include <QList>
#include <QLoggingCategory>
#include <QSettings>

#include "globalshortcutsinterface.h"
#include "shortcutsmanager.h"

namespace AnyKeep {

Q_LOGGING_CATEGORY(logShortcuts, "anykeep.shortcuts")

struct BaseInfo {
    QString name;
    QString defaultKey;
};

static QHash<QString, BaseInfo> shortcuts;

ShortcutsManager::ShortcutsManager(GlobalShortcutsInterface *gs, QObject *parent) : QObject(parent), gs(gs)
{
    shortcuts.insert(QLatin1String(SKNoteFromSelection), { tr("Note From Selection"), QLatin1String("Ctrl+Alt+M") });
}

#if 0
const QMap<QString, QString> &ShortcutsManager::optionsMap() const
{
    static QMap<QString, QString> map;
    if (map.isEmpty()) {
        map.insert(QLatin1String(SKNoteFromSelection), tr("Note From Selection"));
    }
    return map;
}

QAction* ShortcutsManager::shortcut(const QLatin1String &option)
{
    ShortcutAction &sa = shortcuts[option];
    if (!sa.action) {
        sa.action = new QAction(this);
        sa.option = option;
        updateShortcut(sa);
    }
    return sa.action;
}
#endif
QKeySequence ShortcutsManager::key(const QString &option) const
{
    QSettings                           s;
    QString                             opt = QLatin1String("shortcuts.") + option;
    static QHash<QString, QKeySequence> defaults;
    if (!s.contains(opt)) {
        if (defaults.isEmpty()) {
            defaults.insert(QLatin1String(SKNoteFromSelection), QKeySequence("Ctrl+Alt+M"));
        }
        return defaults.value(option);
    }
    const QString stored   = s.value(opt).toString();
    auto          sequence = QKeySequence::fromString(stored, QKeySequence::PortableText);
    if (sequence.isEmpty() && !stored.isEmpty())
        sequence = QKeySequence::fromString(stored, QKeySequence::NativeText);
    return sequence;
}

#if 0
bool ShortcutsManager::updateShortcut(ShortcutAction &sa)
{
    QSettings s;
    QKeySequence seq(s.value("shortcuts." + sa.option).toString());
    if (sa.shortcut.key() == seq) {
        return true;
    }
    /*delete sa.shortcut;
    sa.shortcut = new QxtGlobalShortcut(this);
    connect(sa.shortcut, SIGNAL(activated()), sa.action, SIGNAL(triggered()));*/
    return sa.shortcut.setKey(seq);
}
#endif

bool ShortcutsManager::setKey(const QString &option, const QKeySequence &seq)
{
    QSettings          settings;
    const QString      settingsKey      = QStringLiteral("shortcuts.") + option;
    const QVariant     previous         = settings.value(settingsKey);
    const QKeySequence previousSequence = key(option);
    settings.setValue(settingsKey, seq.toString(QKeySequence::PortableText));
    settings.sync();

    bool backendUpdated = true;
    if (gs && globalActions.contains(option)) {
        if (globals.contains(option)) {
            backendUpdated = gs->updateGlobalShortcut(option, seq);
        } else if (!seq.isEmpty()) {
            // A backend may already know the action even if its initial
            // registration failed (for example because the old key was in
            // conflict). Prefer an explicit update, then fall back to first
            // registration for backends that have not seen the action yet.
            backendUpdated = gs->updateGlobalShortcut(option, seq);
            if (!backendUpdated)
                backendUpdated = gs->registerGlobalShortcut(option, seq, globalActions.value(option));
            if (backendUpdated)
                globals.append(option);
        }
    }
    if (!backendUpdated) {
        if (previous.isValid())
            settings.setValue(settingsKey, previous);
        else
            settings.remove(settingsKey);
        settings.sync();
        if (gs && globalActions.contains(option)) {
            if (globals.contains(option)) {
                gs->updateGlobalShortcut(option, previousSequence);
            } else if (!previousSequence.isEmpty()
                       && gs->registerGlobalShortcut(option, previousSequence, globalActions.value(option))) {
                globals.append(option);
            }
        }
        return false;
    }
    emit shortcutChanged(option);
    return true;
}

QList<ShortcutsManager::ShortcutInfo> ShortcutsManager::all() const
{
    QSettings           s;
    QList<ShortcutInfo> ret;
    foreach (const QString &option, shortcuts.keys()) {
        ret.append({ option, shortcuts[option].name, key(option) });
    }
    return ret;
}

QString ShortcutsManager::friendlyName(const QString &option) const { return shortcuts.value(option).name; }

QString ShortcutsManager::lastError() const { return gs ? gs->lastGlobalShortcutError() : QString(); }

QStringList ShortcutsManager::globalShortcutIds() const { return globalActions.keys(); }

bool ShortcutsManager::registerGlobal(const char *option, QAction *action)
{
    const QString optionName = QLatin1String(option);
    globalActions.insert(optionName, action);

    if (gs) {
        QKeySequence ks = key(option);
        if (!ks.isEmpty()) {
            const bool registered = gs->registerGlobalShortcut(option, ks, action);
            if (registered) {
                globals.append(optionName);
                return true;
            }
        }
    }
    return false;
}

void ShortcutsManager::setShortcutEnable(const QString &option, bool enabled)
{
    if (globals.contains(option) && gs) {
        return gs->setGlobalShortcutEnabled(option, enabled);
    }
}

void ShortcutsManager::triggerGlobal(const QString &option)
{
    auto *action = globalActions.value(option);
    if (!action) {
        qCWarning(logShortcuts) << "triggerGlobal: no action for option" << option;
        return;
    }
    action->trigger();
}

} // namespace AnyKeep
