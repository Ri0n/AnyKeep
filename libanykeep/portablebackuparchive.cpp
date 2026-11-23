#include "portablebackuparchive.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <cmath>
#include <cstring>
#include <limits>

namespace AnyKeep {
namespace {
constexpr int     TarBlockSize = 512;
constexpr qint64  ChunkSize    = 1024 * 1024;
const QString     ManifestPath = QStringLiteral("manifest.json");
const QString     FormatName   = QStringLiteral("anykeep-portable-backup");
const QString     ReadableMode = QStringLiteral("readable");

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool writeExact(QIODevice *device, const char *data, qint64 size)
{
    while (size > 0) {
        const qint64 written = device->write(data, size);
        if (written <= 0)
            return false;
        data += written;
        size -= written;
    }
    return true;
}

bool readExact(QIODevice *device, char *data, qint64 size)
{
    while (size > 0) {
        const qint64 received = device->read(data, size);
        if (received <= 0)
            return false;
        data += received;
        size -= received;
    }
    return true;
}

bool writeOctal(char *field, int width, quint64 value)
{
    const auto digits = QByteArray::number(value, 8);
    if (digits.size() > width - 1)
        return false;
    std::memset(field, '0', size_t(width - 1));
    std::memcpy(field + width - 1 - digits.size(), digits.constData(), size_t(digits.size()));
    field[width - 1] = '\0';
    return true;
}

bool readOctal(const char *field, int width, quint64 *value)
{
    QByteArray text(field, width);
    const int  terminator = text.indexOf('\0');
    if (terminator >= 0)
        text.truncate(terminator);
    text = text.trimmed();
    if (text.isEmpty())
        return false;
    quint64 number = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '7' || number > (std::numeric_limits<quint64>::max() >> 3))
            return false;
        number = number * 8 + quint64(digit - '0');
    }
    *value = number;
    return true;
}

quint64 tarChecksum(const QByteArray &header)
{
    quint64 sum = 0;
    for (int i = 0; i < header.size(); ++i)
        sum += (i >= 148 && i < 156) ? quint64(' ') : quint64(uchar(header.at(i)));
    return sum;
}

bool writeHeader(QIODevice *output, const QString &name, qint64 size)
{
    const QByteArray path = name.toLatin1();
    if (path.isEmpty() || path.size() > 100 || size < 0 || size > PortableBackupArchive::MaxEntryBytes)
        return false;

    QByteArray header(TarBlockSize, '\0');
    std::memcpy(header.data(), path.constData(), size_t(path.size()));
    if (!writeOctal(header.data() + 100, 8, 0644)
        || !writeOctal(header.data() + 108, 8, 0)
        || !writeOctal(header.data() + 116, 8, 0)
        || !writeOctal(header.data() + 124, 12, quint64(size))
        || !writeOctal(header.data() + 136, 12, 0))
        return false;
    header[156] = '0'; // Only ordinary files; never links.
    std::memcpy(header.data() + 257, "ustar", 5);
    std::memcpy(header.data() + 263, "00", 2);
    const quint64 checksum = tarChecksum(header);
    return writeOctal(header.data() + 148, 8, checksum)
        && writeExact(output, header.constData(), header.size());
}

bool writePadding(QIODevice *output, qint64 size)
{
    const qint64 pad = (TarBlockSize - (size % TarBlockSize)) % TarBlockSize;
    const QByteArray zeros(TarBlockSize, '\0');
    return writeExact(output, zeros.constData(), pad);
}

bool readPadding(QIODevice *input, qint64 size)
{
    const int pad = int((TarBlockSize - (size % TarBlockSize)) % TarBlockSize);
    char      bytes[TarBlockSize];
    if (!readExact(input, bytes, pad))
        return false;
    for (int i = 0; i < pad; ++i) {
        if (bytes[i] != '\0')
            return false;
    }
    return true;
}

bool validKind(const QString &kind)
{
    return kind == QStringLiteral("note") || kind == QStringLiteral("attachment")
        || kind == QStringLiteral("draft") || kind == QStringLiteral("folder-catalog")
        || kind == QStringLiteral("settings");
}

bool validMimeType(const QString &mime)
{
    if (mime.size() < 3 || mime.size() > 127 || mime.count(QLatin1Char('/')) != 1)
        return false;
    for (const QChar ch : mime) {
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('/') || ch == QLatin1Char('-')
            || ch == QLatin1Char('_') || ch == QLatin1Char('.')
            || ch == QLatin1Char('+'))
            continue;
        return false;
    }
    return !mime.startsWith(QLatin1Char('/')) && !mime.endsWith(QLatin1Char('/'));
}

bool validMetadata(const PortableBackupEntry &entry)
{
    return PortableBackupArchive::safeEntryPath(entry.path) && entry.path != ManifestPath
        && validKind(entry.kind) && validMimeType(entry.mimeType)
        && entry.size >= 0 && entry.size <= PortableBackupArchive::MaxEntryBytes
        && entry.id.size() <= 256 && entry.noteId.size() <= 256 && entry.providerId.size() <= 256;
}

QJsonObject serializeEntry(const PortableBackupEntry &entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("path"), entry.path);
    object.insert(QStringLiteral("kind"), entry.kind);
    object.insert(QStringLiteral("mimeType"), entry.mimeType);
    object.insert(QStringLiteral("bytes"), double(entry.size));
    object.insert(QStringLiteral("sha256"), QString::fromLatin1(entry.sha256.toHex()));
    if (!entry.id.isEmpty())
        object.insert(QStringLiteral("id"), entry.id);
    if (!entry.noteId.isEmpty())
        object.insert(QStringLiteral("noteId"), entry.noteId);
    if (!entry.providerId.isEmpty())
        object.insert(QStringLiteral("providerId"), entry.providerId);
    return object;
}

bool parseEntry(const QJsonValue &value, PortableBackupEntry *entry)
{
    if (!value.isObject())
        return false;
    const auto object = value.toObject();
    const auto path = object.value(QStringLiteral("path"));
    const auto kind = object.value(QStringLiteral("kind"));
    const auto mime = object.value(QStringLiteral("mimeType"));
    const auto bytes = object.value(QStringLiteral("bytes"));
    const auto sha = object.value(QStringLiteral("sha256"));
    if (!path.isString() || !kind.isString() || !mime.isString()
        || !bytes.isDouble() || !sha.isString())
        return false;

    const double number = bytes.toDouble();
    if (!std::isfinite(number) || number < 0 || number > PortableBackupArchive::MaxEntryBytes
        || std::floor(number) != number)
        return false;
    const auto optionalString = [&object](const QString &key, QString *out) {
        const auto value = object.value(key);
        if (!value.isUndefined() && !value.isString())
            return false;
        *out = value.toString();
        return true;
    };

    entry->path     = path.toString();
    entry->kind     = kind.toString();
    entry->mimeType = mime.toString();
    entry->size     = qint64(number);
    if (!optionalString(QStringLiteral("id"), &entry->id)
        || !optionalString(QStringLiteral("noteId"), &entry->noteId)
        || !optionalString(QStringLiteral("providerId"), &entry->providerId)
        || !validMetadata(*entry))
        return false;

    const QByteArray hex = sha.toString().toLatin1();
    if (hex.size() != 64 || hex != hex.toLower())
        return false;
    for (char ch : hex) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    entry->sha256 = QByteArray::fromHex(hex);
    return entry->sha256.size() == 32;
}

bool parseManifest(const QByteArray &bytes, PortableBackupManifest *manifest)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != FormatName
        || root.value(QStringLiteral("mode")).toString() != ReadableMode
        || root.value(QStringLiteral("version")).toInt(-1) != PortableBackupManifest::CurrentVersion)
        return false;
    const auto list = root.value(QStringLiteral("entries"));
    if (!list.isArray() || list.toArray().size() > PortableBackupArchive::MaxEntries)
        return false;

    PortableBackupManifest parsed;
    QSet<QString> seen;
    for (const auto &item : list.toArray()) {
        PortableBackupEntry entry;
        if (!parseEntry(item, &entry) || seen.contains(entry.path))
            return false;
        seen.insert(entry.path);
        parsed.entries.append(std::move(entry));
    }
    *manifest = std::move(parsed);
    return true;
}

bool readPayload(QIODevice *input, qint64 size, QByteArray *contents, QByteArray *sha256)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(int(ChunkSize), Qt::Uninitialized);
    while (size > 0) {
        const qint64 amount = qMin(size, ChunkSize);
        if (!readExact(input, buffer.data(), amount))
            return false;
        hash.addData(buffer.constData(), amount);
        if (contents)
            contents->append(buffer.constData(), amount);
        size -= amount;
    }
    *sha256 = hash.result();
    return true;
}
} // namespace

bool PortableBackupArchive::safeEntryPath(const QString &path)
{
    if (path.isEmpty() || path.size() > 100 || path == ManifestPath || path.startsWith(QLatin1Char('/')))
        return false;
    const auto parts = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral(".."))
            return false;
    }
    for (const QChar ch : path) {
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('/') || ch == QLatin1Char('-')
            || ch == QLatin1Char('_') || ch == QLatin1Char('.'))
            continue;
        return false;
    }
    return true;
}

bool PortableBackupArchive::writeReadable(QIODevice *output, const QList<PortableBackupSource> &sources, QString *error)
{
    if (!output || !output->isWritable())
        return fail(error, QStringLiteral("Backup destination is not writable"));
    if (sources.size() > MaxEntries)
        return fail(error, QStringLiteral("Too many backup entries"));

    QSet<QString> seen;
    qint64 totalSize = 0;
    for (const auto &source : sources) {
        if (!validMetadata(source.entry) || !source.device || !source.device->isReadable()
            || seen.contains(source.entry.path))
            return fail(error, QStringLiteral("Invalid or duplicate backup entry"));
        seen.insert(source.entry.path);
        if (source.entry.size > MaxTotalBytes - totalSize)
            return fail(error, QStringLiteral("Backup exceeds the size limit"));
        totalSize += source.entry.size;
    }

    QJsonArray entries;
    QByteArray buffer(int(ChunkSize), Qt::Uninitialized);
    for (const auto &source : sources) {
        if (!writeHeader(output, source.entry.path, source.entry.size))
            return fail(error, QStringLiteral("Could not write backup header"));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 remaining = source.entry.size;
        while (remaining > 0) {
            const qint64 count = source.device->read(buffer.data(), qMin(remaining, ChunkSize));
            if (count <= 0)
                return fail(error, QStringLiteral("Backup source became unavailable or was truncated"));
            hash.addData(buffer.constData(), count);
            if (!writeExact(output, buffer.constData(), count))
                return fail(error, QStringLiteral("Could not write backup contents"));
            remaining -= count;
        }
        if (!source.device->atEnd() || !writePadding(output, source.entry.size))
            return fail(error, QStringLiteral("Backup source changed during export"));
        auto entry = source.entry;
        entry.sha256 = hash.result();
        entries.append(serializeEntry(entry));
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), FormatName);
    root.insert(QStringLiteral("version"), PortableBackupManifest::CurrentVersion);
    root.insert(QStringLiteral("mode"), ReadableMode);
    root.insert(QStringLiteral("createdUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("entries"), entries);
    const QByteArray manifest = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (manifest.size() > MaxManifestBytes)
        return fail(error, QStringLiteral("Backup manifest exceeds the size limit"));
    if (!writeHeader(output, ManifestPath, manifest.size())
        || !writeExact(output, manifest.constData(), manifest.size())
        || !writePadding(output, manifest.size()))
        return fail(error, QStringLiteral("Could not write backup manifest"));
    const QByteArray end(TarBlockSize * 2, '\0');
    if (!writeExact(output, end.constData(), end.size()))
        return fail(error, QStringLiteral("Could not finalize backup"));
    return true;
}

bool PortableBackupArchive::inspectReadable(QIODevice *input, PortableBackupManifest *manifest, QString *error)
{
    if (!input || !input->isReadable() || !manifest)
        return fail(error, QStringLiteral("Backup source is not readable"));

    QHash<QString, PortableBackupEntry> observed;
    qint64 totalSize = 0;
    bool hasManifest = false;
    QByteArray manifestBytes;
    while (true) {
        QByteArray header(TarBlockSize, Qt::Uninitialized);
        if (!readExact(input, header.data(), header.size()))
            return fail(error, QStringLiteral("Backup is truncated"));
        if (header == QByteArray(TarBlockSize, '\0')) {
            QByteArray second(TarBlockSize, Qt::Uninitialized);
            if (!readExact(input, second.data(), second.size())
                || second != QByteArray(TarBlockSize, '\0') || !hasManifest || !input->atEnd())
                return fail(error, QStringLiteral("Invalid backup end marker"));
            break;
        }

        quint64 checksum = 0;
        quint64 size = 0;
        if (std::memcmp(header.constData() + 257, "ustar", 5) != 0
            || std::memcmp(header.constData() + 263, "00", 2) != 0
            || header.at(156) != '0'
            || !readOctal(header.constData() + 148, 8, &checksum)
            || checksum != tarChecksum(header)
            || !readOctal(header.constData() + 124, 12, &size))
            return fail(error, QStringLiteral("Invalid or unsupported backup entry header"));

        const QByteArray rawPath(header.constData(), 100);
        const int end = rawPath.indexOf('\0');
        if (end < 0)
            return fail(error, QStringLiteral("Backup entry name is not terminated"));
        const QString path = QString::fromLatin1(rawPath.constData(), end);
        if ((!safeEntryPath(path) && path != ManifestPath) || hasManifest
            || size > quint64(MaxEntryBytes))
            return fail(error, QStringLiteral("Unsafe or unsupported backup entry"));
        if (path == ManifestPath) {
            if (size > quint64(MaxManifestBytes))
                return fail(error, QStringLiteral("Backup manifest is too large"));
            QByteArray checksumBytes;
            if (!readPayload(input, qint64(size), &manifestBytes, &checksumBytes))
                return fail(error, QStringLiteral("Could not read backup manifest"));
            hasManifest = true;
        } else {
            if (observed.size() >= MaxEntries || observed.contains(path) || qint64(size) > MaxTotalBytes - totalSize)
                return fail(error, QStringLiteral("Duplicate entry or backup too large"));
            PortableBackupEntry entry;
            entry.path = path;
            entry.size = qint64(size);
            if (!readPayload(input, entry.size, nullptr, &entry.sha256))
                return fail(error, QStringLiteral("Could not read backup entry"));
            observed.insert(path, std::move(entry));
            totalSize += qint64(size);
        }
        if (!readPadding(input, qint64(size)))
            return fail(error, QStringLiteral("Invalid backup entry padding"));
    }

    PortableBackupManifest parsed;
    if (!parseManifest(manifestBytes, &parsed) || parsed.entries.size() != observed.size())
        return fail(error, QStringLiteral("Invalid or incomplete backup manifest"));
    for (const auto &entry : parsed.entries) {
        const auto found = observed.constFind(entry.path);
        if (found == observed.constEnd() || found->size != entry.size || found->sha256 != entry.sha256)
            return fail(error, QStringLiteral("Backup entry failed its integrity check"));
    }
    *manifest = std::move(parsed);
    return true;
}

} // namespace AnyKeep
