#include "filenoterulestore.h"

#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

namespace AnyKeep {
namespace {

    constexpr quint32 PayloadMagic       = 0x514e5253; // QNRS
    constexpr quint16 PayloadVersion     = 1;
    constexpr quint32 AeadContextSchema  = 1;
    constexpr quint32 MaximumRuleCount   = 100000;
    constexpr quint32 MaximumMarkerCount = 1000000;
    constexpr quint32 MaximumConditions  = 32;
    constexpr quint32 MaximumActions     = 8;

    NoteRuleError error(NoteRuleError::Code code, const QString &message) { return { code, message }; }

    AeadContext context()
    {
        return { KeyDomain::LocalRuleStore, QStringLiteral("anykeep-rule-store"), QStringLiteral("global"),
                 AeadContextSchema, QStringLiteral("rules") };
    }

    QByteArray serialize(const NoteRuleSnapshot &snapshot)
    {
        QByteArray  bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_5_10);
        out << PayloadMagic << PayloadVersion << quint32(snapshot.rules.size()) << quint32(snapshot.markers.size());
        for (const auto &rule : snapshot.rules) {
            out << rule.id << rule.enabled << rule.sortOrder << rule.name << quint8(rule.conditionCombiner)
                << quint32(rule.conditions.size());
            for (const auto &condition : rule.conditions)
                out << quint8(condition.kind) << condition.value << condition.negated;
            out << quint32(rule.actions.size());
            for (const auto &action : rule.actions)
                out << quint8(action.kind) << action.folderId << action.storageId;
            out << rule.stopProcessing << rule.revision << rule.modifiedAt;
        }
        for (const auto &marker : snapshot.markers) {
            out << marker.ruleId << marker.ruleRevision << marker.storageId << marker.noteId << marker.inputFingerprint
                << marker.appliedAt;
        }
        return bytes;
    }

    NoteRuleResult<NoteRuleSnapshot> deserialize(const QByteArray &bytes)
    {
        NoteRuleSnapshot snapshot;
        quint32          magic       = 0;
        quint16          version     = 0;
        quint32          ruleCount   = 0;
        quint32          markerCount = 0;
        QDataStream      in(bytes);
        in.setVersion(QDataStream::Qt_5_10);
        in >> magic >> version >> ruleCount >> markerCount;
        if (magic != PayloadMagic || version != PayloadVersion || ruleCount > MaximumRuleCount
            || markerCount > MaximumMarkerCount) {
            return { {}, error(NoteRuleError::Corrupt, QStringLiteral("Unsupported rule store payload")) };
        }

        snapshot.rules.reserve(int(ruleCount));
        for (quint32 ruleIndex = 0; ruleIndex < ruleCount; ++ruleIndex) {
            NoteRule rule;
            quint8   combiner       = 0;
            quint32  conditionCount = 0;
            in >> rule.id >> rule.enabled >> rule.sortOrder >> rule.name >> combiner >> conditionCount;
            if (conditionCount > MaximumConditions) {
                return { {}, error(NoteRuleError::Corrupt, QStringLiteral("A stored rule has too many conditions")) };
            }
            rule.conditionCombiner = NoteRuleConditionCombiner(combiner);
            rule.conditions.reserve(int(conditionCount));
            for (quint32 conditionIndex = 0; conditionIndex < conditionCount; ++conditionIndex) {
                NoteRuleCondition condition;
                quint8            kind = 0;
                in >> kind >> condition.value >> condition.negated;
                condition.kind = NoteRuleConditionKind(kind);
                rule.conditions.append(std::move(condition));
            }

            quint32 actionCount = 0;
            in >> actionCount;
            if (actionCount > MaximumActions) {
                return { {}, error(NoteRuleError::Corrupt, QStringLiteral("A stored rule has too many actions")) };
            }
            rule.actions.reserve(int(actionCount));
            for (quint32 actionIndex = 0; actionIndex < actionCount; ++actionIndex) {
                NoteRuleAction action;
                quint8         kind = 0;
                in >> kind >> action.folderId >> action.storageId;
                action.kind = NoteRuleActionKind(kind);
                rule.actions.append(std::move(action));
            }
            in >> rule.stopProcessing >> rule.revision >> rule.modifiedAt;
            snapshot.rules.append(std::move(rule));
        }

        snapshot.markers.reserve(int(markerCount));
        for (quint32 markerIndex = 0; markerIndex < markerCount; ++markerIndex) {
            NoteRuleApplicationMarker marker;
            in >> marker.ruleId >> marker.ruleRevision >> marker.storageId >> marker.noteId >> marker.inputFingerprint
                >> marker.appliedAt;
            snapshot.markers.append(std::move(marker));
        }
        if (in.status() != QDataStream::Ok || !in.atEnd())
            return { {}, error(NoteRuleError::Corrupt, QStringLiteral("Invalid rule store payload")) };
        if (const auto validation = NoteRuleEvaluator::validate(snapshot))
            return { {}, { NoteRuleError::Corrupt, validation.message } };
        return { std::move(snapshot), {} };
    }

    QString recoverySuffix()
    {
        return QStringLiteral(".unrecoverable-%1-%2")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz")),
                 QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

} // namespace

FileNoteRuleStore::FileNoteRuleStore(QString filePath, QByteArray masterKey) :
    filePath_(std::move(filePath)), masterKey_(std::move(masterKey))
{
}

bool FileNoteRuleStore::cryptoAvailable() { return SecureEnvelope::isAvailable(); }

NoteRuleResult<NoteRuleSnapshot> FileNoteRuleStore::load() const { return loadPath(filePath_, true); }

NoteRuleResult<NoteRuleSnapshot> FileNoteRuleStore::loadBackup() const { return loadPath(backupFilePath(), false); }

NoteRuleError FileNoteRuleStore::save(const NoteRuleSnapshot &snapshot)
{
    if (const auto keyError = validateKey())
        return keyError;
    if (const auto validation = NoteRuleEvaluator::validate(snapshot))
        return validation;

    if (QFile::exists(filePath_)) {
        const auto existing = readRaw(filePath_);
        if (!existing)
            return existing.error;
        const auto previous = load();
        if (!previous)
            return previous.error;
        if (const auto backupError = writeRaw(backupFilePath(), existing.value))
            return backupError;
    }

    const auto sealed = SecureEnvelope::seal(serialize(snapshot), masterKey_, context());
    if (!sealed)
        return error(NoteRuleError::CryptoUnavailable, sealed.error.message);
    return writeRaw(filePath_, sealed.value);
}

bool FileNoteRuleStore::hasBackup() const { return QFile::exists(backupFilePath()); }

QString FileNoteRuleStore::backupFilePath() const { return filePath_ + QStringLiteral(".bak"); }

NoteRuleError FileNoteRuleStore::restoreBackup(QString *preservedPath)
{
    if (const auto keyError = validateKey())
        return keyError;
    const auto backup = loadBackup();
    if (!backup)
        return backup.error;
    const auto rawBackup = readRaw(backupFilePath());
    if (!rawBackup)
        return rawBackup.error;
    if (const auto quarantineError = quarantineExisting(preservedPath))
        return quarantineError;
    return writeRaw(filePath_, rawBackup.value);
}

NoteRuleError FileNoteRuleStore::recreate(QString *preservedPath)
{
    if (const auto keyError = validateKey())
        return keyError;
    if (const auto quarantineError = quarantineExisting(preservedPath))
        return quarantineError;
    return save({});
}

NoteRuleResult<NoteRuleSnapshot> FileNoteRuleStore::loadPath(const QString &path, bool absentIsEmpty) const
{
    if (const auto keyError = validateKey())
        return { {}, keyError };
    if (!QFile::exists(path)) {
        if (absentIsEmpty)
            return { {}, {} };
        return { {}, error(NoteRuleError::NotFound, QStringLiteral("Rule store backup was not found")) };
    }
    const auto raw = readRaw(path);
    if (!raw)
        return { {}, raw.error };
    const auto opened = SecureEnvelope::open(raw.value, masterKey_, context());
    if (!opened)
        return { {}, error(NoteRuleError::Corrupt, opened.error.message) };
    return deserialize(opened.value);
}

NoteRuleResult<QByteArray> FileNoteRuleStore::readRaw(const QString &path) const
{
    if (path.isEmpty())
        return { {}, error(NoteRuleError::InvalidArgument, QStringLiteral("Rule store path is empty")) };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, error(NoteRuleError::Io, file.errorString()) };
    return { file.readAll(), {} };
}

NoteRuleError FileNoteRuleStore::writeRaw(const QString &path, const QByteArray &bytes) const
{
    if (path.isEmpty())
        return error(NoteRuleError::InvalidArgument, QStringLiteral("Rule store path is empty"));
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return error(NoteRuleError::Io, QStringLiteral("Failed to create the rule store directory"));

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return error(NoteRuleError::Io, file.errorString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return error(NoteRuleError::Io, file.errorString());
    return {};
}

NoteRuleError FileNoteRuleStore::validateKey() const
{
    if (filePath_.isEmpty())
        return error(NoteRuleError::InvalidArgument, QStringLiteral("Rule store path is empty"));
    if (masterKey_.size() != SecureEnvelope::MasterKeySize)
        return error(NoteRuleError::Locked, QStringLiteral("Rule store key is unavailable"));
    if (!cryptoAvailable())
        return error(NoteRuleError::CryptoUnavailable, QStringLiteral("AES-256-GCM is unavailable"));
    return {};
}

NoteRuleError FileNoteRuleStore::quarantineExisting(QString *preservedPath) const
{
    if (preservedPath)
        preservedPath->clear();
    if (!QFile::exists(filePath_) && !QFile::exists(backupFilePath()))
        return {};

    const auto suffix = recoverySuffix();
    QString    preserved;
    if (QFile::exists(filePath_)) {
        preserved = filePath_ + suffix;
        if (!QFile::rename(filePath_, preserved))
            return error(NoteRuleError::Io, QStringLiteral("Failed to preserve the existing rule store"));
    }
    if (QFile::exists(backupFilePath())) {
        const auto backupPreserved = filePath_ + suffix + QStringLiteral(".bak");
        if (!QFile::rename(backupFilePath(), backupPreserved))
            return error(NoteRuleError::Io, QStringLiteral("Failed to preserve the rule store backup"));
        if (preserved.isEmpty())
            preserved = backupPreserved;
    }
    if (preservedPath)
        *preservedPath = preserved;
    return {};
}

} // namespace AnyKeep
