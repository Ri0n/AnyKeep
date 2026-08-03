/*
AnyKeep - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "ptffolderindex.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QUuid>

#include <utility>

namespace AnyKeep {
namespace {

    constexpr int MaximumFolderCount     = 100000;
    constexpr int MaximumAssignmentCount = 1000000;
    constexpr int IndexVersion           = 1;

    FolderCatalogError error(FolderCatalogError::Code code, const QString &message) { return { code, message }; }

    QString recoverySuffix()
    {
        return QStringLiteral(".unrecoverable-%1-%2")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz")),
                 QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    QString encodedUuid(const QUuid &id) { return id.isNull() ? QString {} : id.toString(QUuid::WithoutBraces); }

    QString encodedTime(const QDateTime &time)
    {
        return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString {};
    }

    QJsonObject encodeFolder(const FolderRecord &record)
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), encodedUuid(record.id));
        object.insert(QStringLiteral("parentId"), encodedUuid(record.parentId));
        object.insert(QStringLiteral("name"), record.name);
        object.insert(QStringLiteral("sortOrder"), QString::number(record.sortOrder));
        object.insert(QStringLiteral("collapsed"), record.collapsed);
        object.insert(QStringLiteral("favorite"), record.favorite);
        object.insert(QStringLiteral("archived"), record.archived);
        object.insert(QStringLiteral("revision"), QString::number(record.revision));
        object.insert(QStringLiteral("modifiedAt"), encodedTime(record.modifiedAt));
        object.insert(QStringLiteral("tombstone"), record.tombstone);
        return object;
    }

    QJsonObject encodeAssignment(const NoteFolderAssignment &record)
    {
        QJsonObject object;
        object.insert(QStringLiteral("noteId"), record.noteId);
        object.insert(QStringLiteral("folderId"), encodedUuid(record.folderId));
        object.insert(QStringLiteral("previousFolderId"), encodedUuid(record.previousFolderId));
        object.insert(QStringLiteral("recycledAt"), encodedTime(record.recycledAt));
        object.insert(QStringLiteral("revision"), QString::number(record.revision));
        object.insert(QStringLiteral("modifiedAt"), encodedTime(record.modifiedAt));
        object.insert(QStringLiteral("tombstone"), record.tombstone);
        return object;
    }

    QByteArray serialize(const FolderCatalogSnapshot &snapshot)
    {
        QJsonArray folders;
        for (const auto &record : snapshot.folders)
            folders.append(encodeFolder(record));

        QJsonArray assignments;
        for (const auto &record : snapshot.assignments)
            assignments.append(encodeAssignment(record));

        QJsonObject root;
        root.insert(QStringLiteral("version"), IndexVersion);
        root.insert(QStringLiteral("folders"), folders);
        root.insert(QStringLiteral("assignments"), assignments);
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    bool decodeUuid(const QJsonValue &value, QUuid *id, bool allowNull)
    {
        if (!id || !value.isString())
            return false;
        const auto text = value.toString();
        if (text.isEmpty()) {
            if (!allowNull)
                return false;
            *id = {};
            return true;
        }
        const QUuid decoded(text);
        if (decoded.isNull()) {
            const auto nullUuid = QUuid().toString(QUuid::WithoutBraces);
            if (!allowNull || (text != nullUuid && text != QUuid().toString()))
                return false;
        }
        *id = decoded;
        return true;
    }

    bool decodeSigned(const QJsonValue &value, qint64 *result)
    {
        if (!result || !value.isString())
            return false;
        bool       ok     = false;
        const auto parsed = value.toString().toLongLong(&ok);
        if (!ok)
            return false;
        *result = parsed;
        return true;
    }

    bool decodeUnsigned(const QJsonValue &value, quint64 *result)
    {
        if (!result || !value.isString())
            return false;
        bool       ok     = false;
        const auto parsed = value.toString().toULongLong(&ok);
        if (!ok)
            return false;
        *result = parsed;
        return true;
    }

    bool decodeTime(const QJsonValue &value, QDateTime *result)
    {
        if (!result || !value.isString())
            return false;
        const auto text = value.toString();
        if (text.isEmpty()) {
            *result = {};
            return true;
        }
        const auto time = QDateTime::fromString(text, Qt::ISODateWithMs);
        if (!time.isValid())
            return false;
        *result = time.toUTC();
        return true;
    }

    bool decodeBool(const QJsonObject &object, const QString &key, bool *result)
    {
        const auto value = object.value(key);
        if (!result || !value.isBool())
            return false;
        *result = value.toBool();
        return true;
    }

    FolderCatalogResult<FolderRecord> decodeFolder(const QJsonValue &value)
    {
        if (!value.isObject())
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid PTF folder record")) };

        const auto   object = value.toObject();
        FolderRecord record;
        if (!decodeUuid(object.value(QStringLiteral("id")), &record.id, false)
            || !decodeUuid(object.value(QStringLiteral("parentId")), &record.parentId, true)
            || !object.value(QStringLiteral("name")).isString()
            || !decodeSigned(object.value(QStringLiteral("sortOrder")), &record.sortOrder)
            || !decodeBool(object, QStringLiteral("collapsed"), &record.collapsed)
            || !decodeBool(object, QStringLiteral("favorite"), &record.favorite)
            || !decodeBool(object, QStringLiteral("archived"), &record.archived)
            || !decodeUnsigned(object.value(QStringLiteral("revision")), &record.revision)
            || !decodeTime(object.value(QStringLiteral("modifiedAt")), &record.modifiedAt)
            || !decodeBool(object, QStringLiteral("tombstone"), &record.tombstone)) {
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid PTF folder record fields")) };
        }
        record.name = object.value(QStringLiteral("name")).toString();
        return { record, {} };
    }

    FolderCatalogResult<NoteFolderAssignment> decodeAssignment(const QJsonValue &value, const QString &storageId)
    {
        if (!value.isObject())
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid PTF folder assignment")) };

        const auto           object = value.toObject();
        NoteFolderAssignment record;
        record.storageId = storageId;
        if (!object.value(QStringLiteral("noteId")).isString()
            || !decodeUuid(object.value(QStringLiteral("folderId")), &record.folderId, true)
            || !decodeUnsigned(object.value(QStringLiteral("revision")), &record.revision)
            || !decodeTime(object.value(QStringLiteral("modifiedAt")), &record.modifiedAt)
            || !decodeBool(object, QStringLiteral("tombstone"), &record.tombstone)) {
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid PTF folder assignment fields")) };
        }
        if (object.contains(QStringLiteral("previousFolderId"))
            && !decodeUuid(object.value(QStringLiteral("previousFolderId")), &record.previousFolderId, true)) {
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid PTF recycle folder id")) };
        }
        if (object.contains(QStringLiteral("recycledAt"))
            && !decodeTime(object.value(QStringLiteral("recycledAt")), &record.recycledAt)) {
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid PTF recycle timestamp")) };
        }
        record.noteId = object.value(QStringLiteral("noteId")).toString();
        return { record, {} };
    }

    FolderCatalogResult<FolderCatalogSnapshot> deserialize(const QByteArray &bytes, const QString &storageId)
    {
        QJsonParseError parseError;
        const auto      document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid PTF folder index payload")) };
        }

        const auto root        = document.object();
        const auto version     = root.value(QStringLiteral("version"));
        const auto folders     = root.value(QStringLiteral("folders"));
        const auto assignments = root.value(QStringLiteral("assignments"));
        if (!version.isDouble() || version.toDouble() != IndexVersion || !folders.isArray() || !assignments.isArray()) {
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Unsupported PTF folder index payload")) };
        }

        const auto folderArray     = folders.toArray();
        const auto assignmentArray = assignments.toArray();
        if (folderArray.size() > MaximumFolderCount || assignmentArray.size() > MaximumAssignmentCount) {
            return { {},
                     error(FolderCatalogError::Corrupt, QStringLiteral("PTF folder index exceeds supported limits")) };
        }

        FolderCatalogSnapshot snapshot;
        snapshot.folders.reserve(folderArray.size());
        for (const auto &value : folderArray) {
            const auto decoded = decodeFolder(value);
            if (!decoded)
                return { {}, decoded.error };
            snapshot.folders.append(decoded.value);
        }
        snapshot.assignments.reserve(assignmentArray.size());
        for (const auto &value : assignmentArray) {
            const auto decoded = decodeAssignment(value, storageId);
            if (!decoded)
                return { {}, decoded.error };
            snapshot.assignments.append(decoded.value);
        }

        FolderCatalog catalog;
        if (const auto validation = catalog.replaceSnapshot(snapshot))
            return { {}, { FolderCatalogError::Corrupt, validation.message } };
        return { snapshot, {} };
    }

} // namespace

PtfFolderIndex::PtfFolderIndex(QString filePath, QString storageId) :
    filePath_(std::move(filePath)), storageId_(std::move(storageId))
{
}

FolderCatalogResult<FolderCatalogSnapshot> PtfFolderIndex::load() const { return loadPath(filePath_, true); }

FolderCatalogResult<FolderCatalogSnapshot> PtfFolderIndex::loadBackup() const
{
    return loadPath(backupFilePath(), false);
}

FolderCatalogError PtfFolderIndex::save(const FolderCatalogSnapshot &snapshot) const
{
    if (const auto validation = validateSnapshot(snapshot))
        return validation;

    if (QFile::exists(filePath_)) {
        const auto current = load();
        if (!current)
            return current.error;
        const auto raw = readRaw(filePath_);
        if (!raw)
            return raw.error;
        if (const auto backupError = writeRaw(backupFilePath(), raw.value))
            return backupError;
    }
    return writeRaw(filePath_, serialize(snapshot));
}

QString PtfFolderIndex::backupFilePath() const { return filePath_ + QStringLiteral(".bak"); }

bool PtfFolderIndex::hasBackup() const { return QFile::exists(backupFilePath()); }

FolderCatalogError PtfFolderIndex::restoreBackup(QString *preservedPath) const
{
    if (preservedPath)
        preservedPath->clear();
    const auto backup = loadBackup();
    if (!backup)
        return backup.error;
    const auto raw = readRaw(backupFilePath());
    if (!raw)
        return raw.error;
    if (const auto quarantineError = quarantine(filePath_, preservedPath))
        return quarantineError;
    return writeRaw(filePath_, raw.value);
}

FolderCatalogError PtfFolderIndex::recreate(QString *preservedPath) const
{
    if (preservedPath)
        preservedPath->clear();
    QString preserved;
    if (const auto primaryError = quarantine(filePath_, &preserved))
        return primaryError;
    QString backupPreserved;
    if (const auto backupError = quarantine(backupFilePath(), &backupPreserved))
        return backupError;
    if (preserved.isEmpty())
        preserved = backupPreserved;
    if (preservedPath)
        *preservedPath = preserved;
    return save({});
}

FolderCatalogResult<FolderCatalogSnapshot> PtfFolderIndex::loadPath(const QString &path, bool absentIsEmpty) const
{
    if (path.isEmpty())
        return { {}, error(FolderCatalogError::InvalidArgument, QStringLiteral("PTF folder index path is empty")) };
    if (!QFile::exists(path)) {
        if (absentIsEmpty)
            return { {}, {} };
        return { {}, error(FolderCatalogError::NotFound, QStringLiteral("PTF folder index backup was not found")) };
    }
    const auto raw = readRaw(path);
    if (!raw)
        return { {}, raw.error };
    return deserialize(raw.value, storageId_);
}

FolderCatalogResult<QByteArray> PtfFolderIndex::readRaw(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, error(FolderCatalogError::Io, file.errorString()) };
    return { file.readAll(), {} };
}

FolderCatalogError PtfFolderIndex::writeRaw(const QString &path, const QByteArray &bytes) const
{
    const auto directory = QFileInfo(path).absolutePath();
    if (path.isEmpty() || !QDir().mkpath(directory))
        return error(FolderCatalogError::Io, QStringLiteral("Failed to create PTF folder index directory"));

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return error(FolderCatalogError::Io, file.errorString());
    if (file.write(bytes) != bytes.size() || !file.commit())
        return error(FolderCatalogError::Io, file.errorString());
    return {};
}

FolderCatalogError PtfFolderIndex::validateSnapshot(const FolderCatalogSnapshot &snapshot) const
{
    for (const auto &assignment : snapshot.assignments) {
        if (assignment.storageId != storageId_) {
            return error(FolderCatalogError::InvalidArgument,
                         QStringLiteral("PTF folder index contains an assignment for another storage"));
        }
    }
    FolderCatalog catalog;
    return catalog.replaceSnapshot(snapshot);
}

FolderCatalogError PtfFolderIndex::quarantine(const QString &path, QString *preservedPath) const
{
    if (!QFile::exists(path))
        return {};
    const auto preserved = path + recoverySuffix();
    if (!QFile::rename(path, preserved))
        return error(FolderCatalogError::Io, QStringLiteral("Failed to preserve the PTF folder index"));
    if (preservedPath)
        *preservedPath = preserved;
    return {};
}

} // namespace AnyKeep
