#include "portablebackuparchive.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>

#include <miniz.h>

#include <cstdlib>
#include <limits>

namespace AnyKeep {
namespace {
    constexpr quint64 MaximumExpandedBytes = 64ULL * 1024 * 1024 * 1024;
    constexpr size_t MaximumAllocation = 64 * 1024 * 1024;
    constexpr mz_uint MaximumEntries = 100000;

    PortableBackupInspection fail(const QString &error)
    {
        PortableBackupInspection result;
        result.error = error;
        return result;
    }

    bool running(const PortableBackup::Continue &callback) { return !callback || callback(); }

    bool boundedAllocation(size_t count, size_t size)
    {
        return size == 0 || count <= MaximumAllocation / size;
    }

    struct Zip {
        mz_zip_archive archive {};
        Zip()
        {
            archive.m_pAlloc = [](void *, size_t count, size_t size) -> void * {
                return boundedAllocation(count, size) ? std::malloc(count * size) : nullptr;
            };
            archive.m_pRealloc = [](void *, void *ptr, size_t count, size_t size) -> void * {
                return boundedAllocation(count, size) ? std::realloc(ptr, count * size) : nullptr;
            };
            archive.m_pFree = [](void *, void *ptr) { std::free(ptr); };
        }
        ~Zip() { mz_zip_end(&archive); }
    };

    struct Io {
        QIODevice *device;
        const PortableBackup::Continue &keepGoing;
        bool failed = false;
    };

    size_t readAt(void *opaque, mz_uint64 offset, void *buffer, size_t size)
    {
        auto &io = *static_cast<Io *>(opaque);
        if (!running(io.keepGoing) || offset > quint64(std::numeric_limits<qint64>::max())
            || size > size_t(std::numeric_limits<qint64>::max()) || !io.device->seek(qint64(offset))) {
            io.failed = true;
            return 0;
        }
        const auto count = io.device->read(static_cast<char *>(buffer), qint64(size));
        if (count < 0) {
            io.failed = true;
            return 0;
        }
        return size_t(count);
    }

    size_t writeAt(void *opaque, mz_uint64 offset, const void *buffer, size_t size)
    {
        auto &io = *static_cast<Io *>(opaque);
        if (!running(io.keepGoing) || offset > quint64(std::numeric_limits<qint64>::max())
            || size > size_t(std::numeric_limits<qint64>::max()) || !io.device->seek(qint64(offset))
            || io.device->write(static_cast<const char *>(buffer), qint64(size)) != qint64(size)) {
            io.failed = true;
            return 0;
        }
        return size;
    }

    struct Sink {
        QIODevice *device;
        const PortableBackup::Continue &keepGoing;
        quint64 expected;
        quint64 written = 0;
        QCryptographicHash hash { QCryptographicHash::Sha256 };
    };

    size_t receive(void *opaque, mz_uint64 offset, const void *buffer, size_t size)
    {
        auto &sink = *static_cast<Sink *>(opaque);
        if (!running(sink.keepGoing) || offset != sink.written || size > sink.expected - sink.written)
            return 0;
        const QByteArrayView bytes(static_cast<const char *>(buffer), qsizetype(size));
        if (sink.device && sink.device->write(bytes.data(), bytes.size()) != bytes.size())
            return 0;
        sink.hash.addData(bytes);
        sink.written += size;
        return size;
    }

    bool newPath(const QString &path)
    {
        const QFileInfo info(path);
        return QDir::isAbsolutePath(path) && !info.exists() && !info.isSymLink()
            && QDir(info.absolutePath()).exists();
    }

    // The manifest is the only entry read before validating the complete
    // inventory. Never use arbitrary ZIP paths as filesystem destinations.
    PortableBackupInspection inspectZip(const QString &source, const QString &output,
                                        const PortableBackup::Continue &keepGoing)
    {
        QFile file(source);
        if (!file.open(QIODevice::ReadOnly))
            return fail(file.errorString());
        Io io { &file, keepGoing };
        Zip zip;
        zip.archive.m_pRead = readAt;
        zip.archive.m_pIO_opaque = &io;
        if (!mz_zip_reader_init(&zip.archive, quint64(file.size()), 0))
            return fail(QStringLiteral("Invalid ZIP archive"));

        const auto count = mz_zip_reader_get_num_files(&zip.archive);
        if (!count || count > MaximumEntries)
            return fail(QStringLiteral("Invalid ZIP entry count"));
        QHash<QString, mz_uint> entries;
        quint64 total = 0;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat {};
            if (!running(keepGoing) || !mz_zip_reader_file_stat(&zip.archive, i, &stat)
                || !stat.m_is_supported || stat.m_is_directory || stat.m_is_encrypted
                || stat.m_uncomp_size > MaximumExpandedBytes - total)
                return fail(QStringLiteral("Unsupported or oversized ZIP entry"));
            // Reject Unix symlinks and special files; only regular file
            // contents are ever materialized, without archive attributes.
            const auto type = (stat.m_external_attr >> 16) & 0170000;
            if (type && type != 0100000)
                return fail(QStringLiteral("Non-regular ZIP entry"));
            const auto length = mz_zip_reader_get_filename(&zip.archive, i, nullptr, 0);
            if (length < 2 || length > 4096)
                return fail(QStringLiteral("Invalid ZIP entry name"));
            QByteArray name(int(length), '\0');
            if (mz_zip_reader_get_filename(&zip.archive, i, name.data(), length) != length)
                return fail(QStringLiteral("Invalid ZIP entry name"));
            name.chop(1);
            const auto path = QString::fromUtf8(name);
            if (name.contains('\0') || path.toUtf8() != name || entries.contains(path))
                return fail(QStringLiteral("Invalid or duplicate ZIP entry name"));
            entries.insert(path, i);
            total += stat.m_uncomp_size;
        }

        const auto manifestName = QStringLiteral("manifest.json");
        if (!entries.contains(manifestName))
            return fail(QStringLiteral("ZIP backup manifest is missing"));
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&zip.archive, entries.value(manifestName), &stat)
            || stat.m_uncomp_size > quint64(PortableBackup::MaximumManifestBytes))
            return fail(QStringLiteral("ZIP backup manifest is too large"));
        QByteArray manifest;
        QBuffer buffer(&manifest);
        buffer.open(QIODevice::WriteOnly);
        Sink manifestSink { &buffer, keepGoing, stat.m_uncomp_size };
        if (!mz_zip_reader_extract_to_callback(&zip.archive, entries.take(manifestName), receive, &manifestSink, 0)
            || manifestSink.written != manifestSink.expected)
            return fail(QStringLiteral("Could not read ZIP backup manifest"));
        auto result = PortableBackup::inspectManifest(manifest, keepGoing);
        if (!result)
            return result;
        if (entries.size() != result.files.size())
            return fail(QStringLiteral("ZIP inventory does not match the manifest"));
        for (const auto &entry : result.files) {
            if (!entries.contains(entry.path)
                || !mz_zip_reader_file_stat(&zip.archive, entries.value(entry.path), &stat)
                || stat.m_uncomp_size != quint64(entry.size))
                return fail(QStringLiteral("ZIP inventory does not match: %1").arg(entry.path));
        }

        const QDir root(output);
        if (!output.isEmpty()) {
            QFile out(root.filePath(manifestName));
            if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) || out.write(manifest) != manifest.size()
                || !out.flush())
                return fail(QStringLiteral("Could not write staged manifest"));
            for (const auto &storage : result.storages)
                if (!root.mkpath(storage.directory))
                    return fail(QStringLiteral("Could not create staged storage"));
        }
        for (const auto &entry : result.files) {
            QFile out;
            if (!output.isEmpty()) {
                out.setFileName(root.filePath(entry.path));
                if (!root.mkpath(QFileInfo(out.fileName()).absolutePath())
                    || !out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    return fail(QStringLiteral("Could not create staged file: %1").arg(entry.path));
            }
            Sink sink { output.isEmpty() ? nullptr : &out, keepGoing, quint64(entry.size) };
            if (!mz_zip_reader_extract_to_callback(&zip.archive, entries.value(entry.path), receive, &sink, 0)
                || sink.written != sink.expected || sink.hash.result() != entry.sha256
                || (!output.isEmpty() && !out.flush()))
                return fail(QStringLiteral("ZIP file verification failed: %1").arg(entry.path));
        }
        if (io.failed || !running(keepGoing))
            return fail(QStringLiteral("ZIP reading interrupted"));
        return result;
    }
} // namespace

PortableBackupInspection PortableBackupArchive::create(const QString &source, const QString &destination,
                                                        const PortableBackup::Continue &keepGoing,
                                                        const PortableBackup::Continue &beforePublish)
{
    auto result = PortableBackup::inspectDirectory(source, {}, keepGoing);
    if (!result)
        return result;
    if (!newPath(destination))
        return fail(QStringLiteral("ZIP destination must be a new absolute path"));
    QTemporaryDir staging(QFileInfo(destination).absolutePath() + QStringLiteral("/.anykeep-zip-XXXXXX"));
    if (!staging.isValid())
        return fail(QStringLiteral("Could not create ZIP staging directory"));
    const auto stagedPath = staging.filePath(QStringLiteral("backup.zip"));
    {
        QFile file(stagedPath);
        if (!file.open(QIODevice::ReadWrite | QIODevice::NewOnly))
            return fail(file.errorString());
        Io io { &file, keepGoing };
        Zip zip;
        zip.archive.m_pWrite = writeAt;
        zip.archive.m_pIO_opaque = &io;
        if (!mz_zip_writer_init_v2(&zip.archive, 0, MZ_ZIP_FLAG_WRITE_ZIP64))
            return fail(QStringLiteral("Could not initialize ZIP writer"));
        auto add = [&](const QString &path) {
            QFile input(QDir(source).filePath(path));
            if (!input.open(QIODevice::ReadOnly))
                return false;
            Io inputIo { &input, keepGoing };
            return mz_zip_writer_add_read_buf_callback(&zip.archive, path.toUtf8().constData(), readAt, &inputIo,
                                                       quint64(input.size()), nullptr, nullptr, 0,
                                                       MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0)
                && !inputIo.failed;
        };
        if (!add(QStringLiteral("manifest.json")))
            return fail(QStringLiteral("Could not archive manifest"));
        for (const auto &entry : result.files)
            if (!running(keepGoing) || !add(entry.path))
                return fail(QStringLiteral("Could not archive file: %1").arg(entry.path));
        if (!mz_zip_writer_finalize_archive(&zip.archive) || io.failed || !file.flush())
            return fail(QStringLiteral("Could not finalize ZIP archive"));
    }
    auto verified = inspectZip(stagedPath, {}, keepGoing);
    if (!verified)
        return verified;
    if (verified.manifestSha256 != result.manifestSha256)
        return fail(QStringLiteral("Source manifest changed while archiving"));
    if (!running(keepGoing) || !running(beforePublish) || !QFile::rename(stagedPath, destination))
        return fail(QStringLiteral("Could not publish ZIP archive"));
    return verified;
}

PortableBackupInspection PortableBackupArchive::extract(const QString &source, const QString &privateDestination,
                                                         const PortableBackup::Continue &keepGoing,
                                                         const PortableBackup::Continue &beforePublish)
{
    if (!newPath(privateDestination))
        return fail(QStringLiteral("ZIP extraction destination must be a new absolute path"));
    QTemporaryDir staging(QFileInfo(privateDestination).absolutePath() + QStringLiteral("/.anykeep-unzip-XXXXXX"));
    if (!staging.isValid())
        return fail(QStringLiteral("Could not create private ZIP staging directory"));
    auto result = inspectZip(source, staging.path(), keepGoing);
    if (!result)
        return result;
    auto verified = PortableBackup::inspectDirectory(staging.path(), {}, keepGoing);
    if (!verified)
        return verified;
    if (verified.manifestSha256 != result.manifestSha256 || !running(keepGoing) || !running(beforePublish)
        || !QDir().rename(staging.path(), privateDestination))
        return fail(QStringLiteral("Could not publish extracted ZIP tree"));
    staging.setAutoRemove(false);
    return verified;
}

} // namespace AnyKeep
