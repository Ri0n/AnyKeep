/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef PTFFOLDERINDEX_H
#define PTFFOLDERINDEX_H

#include "foldercatalog.h"

#include <QByteArray>
#include <QString>

namespace QtNote {

/**
 * Versioned, atomic JSON persistence for the portable PTF folder catalog.
 *
 * PTF note bodies intentionally remain at the storage root: moving a note
 * between folders updates only this index and therefore keeps note ids, media
 * sidecars and externally managed files stable.
 */
class PtfFolderIndex final {
public:
    PtfFolderIndex(QString filePath, QString storageId);

    FolderCatalogResult<FolderCatalogSnapshot> load() const;
    FolderCatalogResult<FolderCatalogSnapshot> loadBackup() const;
    FolderCatalogError                         save(const FolderCatalogSnapshot &snapshot) const;

    QString filePath() const { return filePath_; }
    QString backupFilePath() const;
    bool    hasBackup() const;

    /// Restores a verified backup while retaining the replaced primary under a
    /// timestamped recovery name. This is deliberately an explicit action.
    FolderCatalogError restoreBackup(QString *preservedPath = nullptr) const;
    /// Preserves any unreadable index data and starts with an empty index.
    FolderCatalogError recreate(QString *preservedPath = nullptr) const;

private:
    QString filePath_;
    QString storageId_;

    FolderCatalogResult<FolderCatalogSnapshot> loadPath(const QString &path, bool absentIsEmpty) const;
    FolderCatalogResult<QByteArray>            readRaw(const QString &path) const;
    FolderCatalogError                         writeRaw(const QString &path, const QByteArray &bytes) const;
    FolderCatalogError                         validateSnapshot(const FolderCatalogSnapshot &snapshot) const;
    FolderCatalogError                         quarantine(const QString &path, QString *preservedPath) const;
};

} // namespace QtNote

#endif // PTFFOLDERINDEX_H
