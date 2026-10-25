/*
    SPDX-License-Identifier: GPL-3.0-only
*/

#ifndef ANYKEEP_DBUS_H
#define ANYKEEP_DBUS_H

#include "anykeep_export.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

namespace AnyKeep {

class Main;
class GlobalNoteFinder;

class ANYKEEP_EXPORT AnyKeepDBus : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.ri0n.AnyKeep")

public:
    explicit AnyKeepDBus(Main *anykeep, QObject *parent = nullptr);
    ~AnyKeepDBus() override;

signals:
    Q_SCRIPTABLE void notesChanged();
    Q_SCRIPTABLE void bodySearchFinished(const QString &query);
    Q_SCRIPTABLE void storageConnectivityChanged();
    Q_SCRIPTABLE void stickyNotesChanged();
    Q_SCRIPTABLE void globalShortcutsChanged();

    void createNoteRequested();
    void globalShortcutActivated(const QString &id);
    void noteManagerRequested();
    void optionsRequested();
    void aboutRequested();
    void quitRequested();
    void openNoteRequested(const QString &storageId, const QString &noteId);

public slots:
    Q_SCRIPTABLE QString notesJson(int offset, int limit, const QString &query) const;
    Q_SCRIPTABLE QString notesWithBodyJson(int offset, int limit, const QString &query);
    Q_SCRIPTABLE QString storageConnectivityJson() const;
    Q_SCRIPTABLE QString globalShortcutsJson() const;
    Q_SCRIPTABLE QString stickyNotesJson() const;
    Q_SCRIPTABLE QString stickyNoteJson(const QString &stickyId) const;
    Q_SCRIPTABLE QString stickyNoteForPresentationJson(const QString &presentationId) const;
    Q_SCRIPTABLE QString claimWindowGeometry();
    Q_SCRIPTABLE void    storeWindowGeometry(const QString &key, int x, int y, int width, int height);
    Q_SCRIPTABLE void    windowGeometryScriptReady();
    Q_SCRIPTABLE void    setXdgActivationToken(const QString &token);
    Q_SCRIPTABLE void    openNote(const QString &storageId, const QString &noteId);
    Q_SCRIPTABLE void    createNote();
    Q_SCRIPTABLE void    openStickyNote(const QString &stickyId);
    Q_SCRIPTABLE void    unpinStickyNote(const QString &stickyId);
    Q_SCRIPTABLE void    unpinStickyNoteForPresentation(const QString &presentationId);
    Q_SCRIPTABLE void    activateGlobalShortcut(const QString &id);
    Q_SCRIPTABLE void    showNoteManager();
    Q_SCRIPTABLE void    showOptions();
    Q_SCRIPTABLE void    showAbout();
    Q_SCRIPTABLE void    quit();

private:
    struct BodySearch {
        QSet<QString>              matches;
        QPointer<GlobalNoteFinder> finder;
        quint64                    lastUsed { 0 };
    };

    QString menuNotesJson(int offset, int limit, const QString &query, bool searchInBody,
                          const QSet<QString> &bodyMatches, bool searching) const;
    void    clearBodySearches();

    QHash<QString, BodySearch> m_bodySearches;
    quint64                    m_bodySearchSerial { 0 };
    Main                      *m_anykeep;
    bool                       m_registeredService = false;
};

} // namespace AnyKeep

#endif // ANYKEEP_DBUS_H
