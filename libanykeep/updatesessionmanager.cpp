#include "updatesessionmanager.h"

#include <QSettings>

namespace AnyKeep {

namespace {
    constexpr int  SessionSchemaVersion = 1;
    constexpr auto SessionGroup         = "update-session";
    constexpr auto UpdatesGroup         = "updates";
    constexpr auto LastStartedVersion   = "lastStartedVersion";
}

std::optional<UpdateSessionState> UpdateSessionManager::pendingSession() const
{
    QSettings settings;
    settings.beginGroup(QLatin1String(SessionGroup));
    if (settings.value(QStringLiteral("schema")).toInt() != SessionSchemaVersion
        || !settings.value(QStringLiteral("pending"), false).toBool()) {
        settings.endGroup();
        return std::nullopt;
    }

    UpdateSessionState session;
    session.sourceVersion          = settings.value(QStringLiteral("sourceVersion")).toString();
    session.targetVersion          = settings.value(QStringLiteral("targetVersion")).toString();
    session.noteManagerVisible     = settings.value(QStringLiteral("noteManagerVisible"), false).toBool();
    session.noteManagerGeometry    = settings.value(QStringLiteral("noteManagerGeometry")).toRect();
    session.noteManagerStorageId   = settings.value(QStringLiteral("noteManagerStorageId")).toString();
    session.noteManagerNoteId      = settings.value(QStringLiteral("noteManagerNoteId")).toString();
    session.noteManagerDraftId     = QUuid(settings.value(QStringLiteral("noteManagerDraftId")).toString());
    session.restoreNoteManagerNote = settings.value(QStringLiteral("restoreNoteManagerNote"), false).toBool();
    session.optionsVisible         = settings.value(QStringLiteral("optionsVisible"), false).toBool();
    session.optionsGeometry        = settings.value(QStringLiteral("optionsGeometry")).toRect();

    const int count = settings.beginReadArray(QStringLiteral("noteWindows"));
    session.noteWindows.reserve(count);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        UpdateSessionNoteWindow note;
        note.storageId = settings.value(QStringLiteral("storageId")).toString();
        note.noteId    = settings.value(QStringLiteral("noteId")).toString();
        note.draftId   = QUuid(settings.value(QStringLiteral("draftId")).toString());
        note.geometry  = settings.value(QStringLiteral("geometry")).toRect();
        if (!note.storageId.isEmpty() || !note.draftId.isNull())
            session.noteWindows.append(note);
    }
    settings.endArray();
    settings.endGroup();
    return session;
}

bool UpdateSessionManager::savePendingSession(const UpdateSessionState &session) const
{
    QSettings settings;
    settings.beginGroup(QLatin1String(SessionGroup));
    settings.remove(QString());
    settings.setValue(QStringLiteral("schema"), SessionSchemaVersion);
    settings.setValue(QStringLiteral("pending"), true);
    settings.setValue(QStringLiteral("sourceVersion"), session.sourceVersion);
    settings.setValue(QStringLiteral("targetVersion"), session.targetVersion);
    settings.setValue(QStringLiteral("noteManagerVisible"), session.noteManagerVisible);
    settings.setValue(QStringLiteral("noteManagerGeometry"), session.noteManagerGeometry);
    settings.setValue(QStringLiteral("noteManagerStorageId"), session.noteManagerStorageId);
    settings.setValue(QStringLiteral("noteManagerNoteId"), session.noteManagerNoteId);
    settings.setValue(QStringLiteral("noteManagerDraftId"), session.noteManagerDraftId.toString(QUuid::WithoutBraces));
    settings.setValue(QStringLiteral("restoreNoteManagerNote"), session.restoreNoteManagerNote);
    settings.setValue(QStringLiteral("optionsVisible"), session.optionsVisible);
    settings.setValue(QStringLiteral("optionsGeometry"), session.optionsGeometry);

    settings.beginWriteArray(QStringLiteral("noteWindows"), session.noteWindows.size());
    for (int i = 0; i < session.noteWindows.size(); ++i) {
        settings.setArrayIndex(i);
        const auto &note = session.noteWindows.at(i);
        settings.setValue(QStringLiteral("storageId"), note.storageId);
        settings.setValue(QStringLiteral("noteId"), note.noteId);
        settings.setValue(QStringLiteral("draftId"), note.draftId.toString(QUuid::WithoutBraces));
        settings.setValue(QStringLiteral("geometry"), note.geometry);
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void UpdateSessionManager::clearPendingSession() const
{
    QSettings settings;
    settings.beginGroup(QLatin1String(SessionGroup));
    settings.remove(QString());
    settings.endGroup();
    settings.sync();
}

QString UpdateSessionManager::recordStartedVersion(const QString &currentVersion) const
{
    QSettings settings;
    settings.beginGroup(QLatin1String(UpdatesGroup));
    const QString previousVersion = settings.value(QLatin1String(LastStartedVersion)).toString();
    settings.setValue(QLatin1String(LastStartedVersion), currentVersion);
    settings.endGroup();
    settings.sync();
    return previousVersion;
}

} // namespace AnyKeep
