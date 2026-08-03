/*
AnyKeep - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Contacts:
E-Mail: rion4ik@gmail.com XMPP: rion@jabber.ru
*/

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QXmlStreamReader>

#include "notedata.h"
#include "tomboynoteformat.h"
#include "tomboystorage.h"

namespace AnyKeep {

namespace {

    bool parseTomboyXml(QIODevice *device, QDomDocument &dom)
    {
        QXmlStreamReader   reader(device);
        QList<QDomElement> elementStack;

        while (!reader.atEnd()) {
            reader.readNext();

            if (reader.isStartElement()) {
                auto element = dom.createElement(reader.qualifiedName().toString());
                for (const auto &attribute : reader.attributes())
                    element.setAttribute(attribute.qualifiedName().toString(), attribute.value().toString());

                if (elementStack.isEmpty())
                    dom.appendChild(element);
                else
                    elementStack.last().appendChild(element);
                elementStack.append(element);
                continue;
            }

            if (reader.isEndElement()) {
                if (elementStack.isEmpty())
                    return false;
                elementStack.removeLast();
                continue;
            }

            if (reader.isCharacters()) {
                if (!elementStack.isEmpty())
                    elementStack.last().appendChild(dom.createTextNode(reader.text().toString()));
                continue;
            }

            if (reader.isComment()) {
                const auto comment = dom.createComment(reader.text().toString());
                if (elementStack.isEmpty())
                    dom.appendChild(comment);
                else
                    elementStack.last().appendChild(comment);
                continue;
            }

            if (reader.isProcessingInstruction()) {
                dom.appendChild(dom.createProcessingInstruction(reader.processingInstructionTarget().toString(),
                                                                reader.processingInstructionData().toString()));
            }
        }

        return !reader.hasError() && elementStack.isEmpty() && !dom.documentElement().isNull();
    }

    QString nodeText(const QDomNode &node)
    {
        QString text;
        for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (child.isText())
                text += child.nodeValue();
            else if (child.isElement())
                text += nodeText(child);
        }
        return text;
    }

    QStringList xmlTags(const QDomNode &node)
    {
        QStringList tags;
        for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (child.isElement() && child.nodeName() == QLatin1String("tag")) {
                const auto tag = nodeText(child).trimmed();
                if (!tag.isEmpty() && !tags.contains(tag))
                    tags.append(tag);
            }
        }
        return tags;
    }

    QStringList searchableTags(const QStringList &tags)
    {
        QStringList result;
        for (const auto &tag : tags) {
            if (!tag.startsWith(QLatin1String("system:")) && !result.contains(tag))
                result.append(tag);
        }
        return result;
    }

    bool readNoteFile(const QString &fileName, Note &note)
    {
        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly))
            return false;

        QDomDocument dom(QStringLiteral("TomboyData"));
        if (!parseTomboyXml(&file, dom))
            return false;

        const auto root = dom.documentElement();
        const auto tags = xmlTags(root.namedItem(QStringLiteral("tags")));
        note.setId(QFileInfo(fileName).completeBaseName());
        const auto title = nodeText(root.namedItem(QStringLiteral("title")));
        const auto content
            = root.namedItem(QStringLiteral("text")).namedItem(QStringLiteral("note-content")).toElement();
        note.setTitle(title);
        note.setText(TomboyNoteFormat::markdownFromContent(content, title), Note::Markdown);
        note.setTags(searchableTags(tags));
        note.setLastChangeUTC(
            QDateTime::fromString(nodeText(root.namedItem(QStringLiteral("last-change-date"))), Qt::ISODate));
        note.setBackendValue(QStringLiteral("fileName"), fileName);
        note.setBackendValue(QStringLiteral("xmlTags"), tags);
        note.setBackendValue(QStringLiteral("createDate"), nodeText(root.namedItem(QStringLiteral("create-date"))));
        note.setBackendValue(QStringLiteral("cursorPosition"),
                             nodeText(root.namedItem(QStringLiteral("cursor-position"))).toInt());
        note.setBackendValue(QStringLiteral("selectionBoundPosition"),
                             nodeText(root.namedItem(QStringLiteral("selection-bound-position"))).toInt());
        note.setBackendValue(QStringLiteral("width"), nodeText(root.namedItem(QStringLiteral("width"))).toInt());
        note.setBackendValue(QStringLiteral("height"), nodeText(root.namedItem(QStringLiteral("height"))).toInt());
        note.setBackendValue(QStringLiteral("x"), nodeText(root.namedItem(QStringLiteral("x"))).toInt());
        note.setBackendValue(QStringLiteral("y"), nodeText(root.namedItem(QStringLiteral("y"))).toInt());
        note.setBackendValue(QStringLiteral("openOnStartup"),
                             nodeText(root.namedItem(QStringLiteral("open-on-startup"))));
        return true;
    }

    bool writeNoteFile(const QString &fileName, const Note &note)
    {
        QDomDocument dom;
        dom.appendChild(dom.createProcessingInstruction(QStringLiteral("xml"),
                                                        QStringLiteral("version=\"1.0\" encoding=\"utf-8\"")));
        auto root = dom.createElement(QStringLiteral("note"));
        dom.appendChild(root);
        root.setAttribute(QStringLiteral("version"), QStringLiteral("0.3"));
        root.setAttribute(QStringLiteral("xmlns:link"), QStringLiteral("http://beatniksoftware.com/tomboy/link"));
        root.setAttribute(QStringLiteral("xmlns:size"), QStringLiteral("http://beatniksoftware.com/tomboy/size"));
        root.setAttribute(QStringLiteral("xmlns"), QStringLiteral("http://beatniksoftware.com/tomboy"));

        auto title = dom.createElement(QStringLiteral("title"));
        title.appendChild(dom.createTextNode(note.title()));
        root.appendChild(title);

        auto text = dom.createElement(QStringLiteral("text"));
        text.setAttribute(QStringLiteral("xml:space"), QStringLiteral("preserve"));
        auto content = dom.createElement(QStringLiteral("note-content"));
        content.setAttribute(QStringLiteral("version"), QStringLiteral("0.1"));
        TomboyNoteFormat::appendMarkdownContent(dom, content, note.title(), note.text());
        text.appendChild(content);
        root.appendChild(text);

        auto tags = note.backendValue(QStringLiteral("xmlTags")).toStringList();
        for (const auto &tag : note.tags()) {
            if (!tags.contains(tag))
                tags.append(tag);
        }
        if (!tags.isEmpty()) {
            auto tagsNode = dom.createElement(QStringLiteral("tags"));
            for (const auto &tag : tags) {
                auto tagNode = dom.createElement(QStringLiteral("tag"));
                tagNode.appendChild(dom.createTextNode(tag));
                tagsNode.appendChild(tagNode);
            }
            root.appendChild(tagsNode);
        }

        const auto requestedModified
            = note.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).toDateTime();
        const auto modified    = requestedModified.isValid() ? requestedModified : QDateTime::currentDateTimeUtc();
        const auto appendValue = [&dom, &root](const QString &name, const QString &value) {
            auto element = dom.createElement(name);
            element.appendChild(dom.createTextNode(value));
            root.appendChild(element);
        };
        const auto modifiedText = modified.toUTC().toString(Qt::ISODateWithMs);
        appendValue(QStringLiteral("last-change-date"), modifiedText);
        appendValue(QStringLiteral("last-metadata-change-date"), modifiedText);
        const auto createDate = note.backendValue(QStringLiteral("createDate")).toString();
        appendValue(QStringLiteral("create-date"), createDate.isEmpty() ? modifiedText : createDate);

        const auto positiveBackendInt = [&note](const QString &key, int fallback) {
            const int value = note.backendValue(key).toInt();
            return value > 0 ? value : fallback;
        };
        const int cursorPosition = positiveBackendInt(QStringLiteral("cursorPosition"), 1);
        appendValue(QStringLiteral("cursor-position"), QString::number(cursorPosition));
        appendValue(QStringLiteral("selection-bound-position"),
                    QString::number(positiveBackendInt(QStringLiteral("selectionBoundPosition"), cursorPosition)));
        appendValue(QStringLiteral("width"), QString::number(positiveBackendInt(QStringLiteral("width"), 337)));
        appendValue(QStringLiteral("height"), QString::number(positiveBackendInt(QStringLiteral("height"), 200)));
        appendValue(QStringLiteral("x"),
                    QString::number(note.backendValue(QStringLiteral("x")).isValid()
                                        ? note.backendValue(QStringLiteral("x")).toInt()
                                        : 100));
        appendValue(QStringLiteral("y"),
                    QString::number(note.backendValue(QStringLiteral("y")).isValid()
                                        ? note.backendValue(QStringLiteral("y")).toInt()
                                        : 100));
        const QString openOnStartup = note.backendValue(QStringLiteral("openOnStartup")).toString();
        appendValue(QStringLiteral("open-on-startup"),
                    openOnStartup.compare(QLatin1String("True"), Qt::CaseInsensitive) == 0 ? QStringLiteral("True")
                                                                                           : QStringLiteral("False"));

        QFile file(fileName);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(dom.toByteArray(2)) >= 0;
    }

} // namespace

static QString newId()
{
    QString uid = QUuid::createUuid().toString().toLower();
    if (uid[0] == '{') {
        return uid.mid(1, uid.length() - 2);
    }
    return uid;
}

TomboyStorage::TomboyStorage(QObject *parent) : FileStorage(parent) { fileExt.append("note"); }

bool TomboyStorage::init()
{
    auto path = QSettings().value("storage.tomboy.path").toString();
    path      = path.isEmpty() ? findStorageDir() : path;
    if (path.isEmpty())
        return false;
    notesDir.setPath(path);
    return isAccessible();
}

bool TomboyStorage::isAccessible() const { return notesDir.isReadable(); }

const QString TomboyStorage::systemName() const { return storageId; }

const QString TomboyStorage::name() const { return tr("Tomboy Storage"); }

QIcon TomboyStorage::storageIcon() const { return QIcon(":/icons/tomboy"); }

QIcon TomboyStorage::noteIcon() const { return QIcon(":/icons/tomboynote"); }

QList<Note> TomboyStorage::noteListFromInfoList(const QFileInfoList &files)
{
    QList<Note> ret;
    foreach (const QFileInfo &fi, files) {
        Note note(new NoteData(this));
        if (readNoteFile(fi.canonicalFilePath(), note)) {
            ret.append(note);
        }
    }
    return ret;
}

Note TomboyStorage::createNote()
{
    Note note(new NoteData(this));
    note.setText(QString(), Note::Markdown);
    return note;
}

Note TomboyStorage::note(const QString &id)
{
    if (!id.isEmpty()) {
        QString   fileName = notesDir.absoluteFilePath(QString("%1.note").arg(id));
        QFileInfo fi(fileName);
        if (fi.isReadable()) {
            Note result(new NoteData(this));
            result.setId(id);
            result.setBackendValue(QStringLiteral("fileName"), fileName);
            if (loadNote(result))
                return result;
        }
        handleFSError();
    }
    return Note();
}

bool TomboyStorage::loadNote(Note &note)
{
    auto fileName = note.backendValue(QStringLiteral("fileName")).toString();
    if (fileName.isEmpty() && !note.id().isEmpty())
        fileName = notesDir.absoluteFilePath(note.id() + QStringLiteral(".note"));
    if (readNoteFile(fileName, note))
        return true;
    handleFSError();
    return false;
}

bool TomboyStorage::saveNote(const Note &note)
{
    // availableFormats returns just markdown, so format is markdown
    QString    oldNoteId         = note.id();
    const bool existedBeforeSave = noteFileExists(oldNoteId);
    QString    newNoteId         = oldNoteId.isEmpty() ? newId() : oldNoteId;

    auto       baseName = QString(QLatin1String("%1.note")).arg(newNoteId);
    const auto fileName = notesDir.absoluteFilePath(baseName);
    if (!writeNoteFile(fileName, note)) {
        handleFSError();
        return false;
    }
    Note saved = note;
    saved.setId(newNoteId);
    const auto requestedModified
        = note.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).toDateTime();
    saved.setLastChangeUTC(requestedModified.isValid() ? requestedModified : QDateTime::currentDateTimeUtc());
    saved.removeBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey));
    saved.setBackendValue(QStringLiteral("fileName"), fileName);
    notifyNoteSaved(saved, oldNoteId, existedBeforeSave);
    return true;
}

QList<Note::Format> TomboyStorage::availableFormats() const
{
    static auto formats = QList<Note::Format>() << Note::Markdown;
    // Tomboy XML conversion supports the compatible Markdown subset on load/save.
    return formats;
}

QString TomboyStorage::findStorageDir() const
{
    QStringList tomboyDirs;

    QString dataLocation = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#ifdef Q_OS_UNIX
    // Tomboy-ng uses this XDG data directory by default. Prefer it over the
    // legacy Tomboy locations when more than one installation is present.
    tomboyDirs << (dataLocation + QLatin1String("/tomboy-ng"));
    tomboyDirs << (dataLocation + QLatin1String("/tomboy"));
    tomboyDirs << (QDir::home().path() + "/.tomboy");
#elif defined(Q_OS_MAC)
    tomboyDirs << (dataLocation + "/Tomboy");
    tomboyDirs << (QDir::homePath() + "/.config/tomboy/");
#elif defined(Q_OS_WIN)
    tomboyDirs << (dataLocation + "/Tomboy/notes");
    tomboyDirs << (dataLocation + "/tomboy");
#endif
    foreach (const QString &d, tomboyDirs) {
        QFileInfo fi(d);
        if (fi.isDir() && fi.isWritable()) {
            return d;
        }
    }
    return QString();
}

QString TomboyStorage::storageId = QStringLiteral("tomboy");

} // namespace AnyKeep
