/*
AnyKeep - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef FOLDEROPERATIONSCONTROLLER_H
#define FOLDEROPERATIONSCONTROLLER_H

#include "anykeep_export.h"
#include "foldercatalog.h"
#include "note.h"

#include <QObject>

namespace AnyKeep {

class FolderCatalogManager;
class NoteManager;
class NoteStorage;

/**
 * Coordinates one note-to-folder operation across the global encrypted
 * overlay and an optional native provider implementation.
 *
 * The overlay is committed first.  This keeps folder organisation available
 * for Tomboy and protects a desired move while a remote/native operation is
 * unavailable.  A native catalog sync deliberately contains the provider's
 * current assignments, not the newly staged one: a dirty editor can prepare
 * the tree without publishing its folder assignment before its draft is
 * actually published.
 */
class ANYKEEP_EXPORT FolderOperationsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    /** Shared controller for operations started outside a workspace window. */
    static FolderOperationsController *instance();

    explicit FolderOperationsController(FolderCatalogManager *catalogManager = nullptr,
                                        NoteManager *noteManager = nullptr, QObject *parent = nullptr);

    bool    busy() const { return pendingOperations_ > 0; }
    QString errorString() const { return errorString_; }

    /**
     * Persist the overlay and, where possible, request a native metadata-only
     * move. An empty folder ID represents Unsorted.
     */
    bool assignNoteFolder(const QString &storageId, const QString &noteId, const QUuid &folderId,
                          bool overlayAlreadyStored = false);
    /** Persist only the global encrypted overlay; used for a folder change kept in a draft. */
    bool storeOverlayAssignment(const QString &storageId, const QString &noteId, const QUuid &folderId);
    /** Ensure a native provider knows the current tree without changing note assignments. */
    bool prepareNativeFolderTree(const QString &storageId);
    /** Prepare every currently available provider that can persist a folder tree. */
    bool prepareNativeFolderTrees();

signals:
    void busyChanged();
    void errorStringChanged();
    void assignmentFinished(const QString &storageId, const QString &noteId, const QUuid &folderId, bool nativeStored);
    void nativeTreePrepared(const QString &storageId, bool success);

private:
    FolderCatalogManager *catalogManager_ { nullptr };
    NoteManager          *noteManager_ { nullptr };
    int                   pendingOperations_ { 0 };
    QString               errorString_;

    void                                       setError(const QString &error);
    void                                       beginOperation();
    void                                       endOperation();
    FolderCatalogResult<FolderCatalogSnapshot> nativeTreeSnapshot(NoteStorage *storage) const;
    bool updateOverlay(const QString &storageId, const QString &noteId, const QUuid &folderId);
    void startNativeAssignment(NoteStorage *storage, const Note &note, const QUuid &folderId);
    void startNativeTreePreparation(NoteStorage *storage, bool assignmentFollows, Note note = {}, QUuid folderId = {});
    void finishAssignment(const QString &storageId, const QString &noteId, const QUuid &folderId, bool nativeStored,
                          const QString &error = {});
};

} // namespace AnyKeep

#endif // FOLDEROPERATIONSCONTROLLER_H
