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

// #include "note.h"
#include "draftmanager.h"
#include "notedata.h"
#include "notemanager.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextImageFormat>
#include <QUrl>

namespace AnyKeep {

namespace {
    QString htmlAttribute(const QString &attributes, const QString &name)
    {
        const QRegularExpression expression(
            QStringLiteral(R"((?:^|\s)%1\s*=\s*(["'])(.*?)\1)").arg(QRegularExpression::escape(name)),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        const auto match = expression.match(attributes);
        return match.hasMatch() ? QTextDocumentFragment::fromHtml(match.captured(2)).toPlainText() : QString();
    }

    QString audioDisplayTitle(const QString &source)
    {
        static const QRegularExpression audio(QStringLiteral(R"(^\s*<audio\b([^>]*)>.*?</audio>\s*$)"),
                                              QRegularExpression::CaseInsensitiveOption
                                                  | QRegularExpression::DotMatchesEverythingOption);
        const auto                      match = audio.match(source);
        if (!match.hasMatch())
            return {};

        QString result = htmlAttribute(match.captured(1), QStringLiteral("title")).trimmed();
        if (result.isEmpty()) {
            const QString mediaSource = htmlAttribute(match.captured(1), QStringLiteral("src")).trimmed();
            result                    = QFileInfo(QUrl(mediaSource).path()).fileName();
        }
        return result.trimmed();
    }

    QString htmlImageDisplayTitle(const QString &source)
    {
        if (!source.contains(QStringLiteral("<img"), Qt::CaseInsensitive))
            return {};

        QTextDocument document;
        document.setHtml(source);
        QString result;
        bool    foundImage = false;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
            for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
                const QTextFragment textFragment = fragment.fragment();
                if (!textFragment.isValid() || !textFragment.charFormat().isImageFormat())
                    continue;
                foundImage             = true;
                const auto imageFormat = textFragment.charFormat().toImageFormat();
                QString    label       = imageFormat.property(QTextFormat::ImageAltText).toString();
                if (label.isEmpty())
                    label = imageFormat.property(QTextFormat::ImageTitle).toString();
                if (label.isEmpty())
                    label = QFileInfo(QUrl(imageFormat.name()).path()).fileName();
                result += label;
            }
        }
        return foundImage ? result.trimmed() : QString();
    }
} // namespace

Note::Note() { }

Note::~Note() { }

Note::Note(NoteData *data) : d(QExplicitlySharedDataPointer<NoteData>(data)) { }

Note::Note(const Note &note) : d(note.d) { }

Note::Note(Note &&note) : d(std::move(note.d)) { }

Note &Note::operator=(const Note &note)
{
    d = note.d;
    return *this;
}

bool Note::isNull() const { return !d; }

bool Note::isEmpty() const { return isNull() || (d->title_.isEmpty() && d->text_.isEmpty()); }

bool Note::isLoaded() const { return !isNull() && d->loaded_; }

bool Note::save()
{
    auto storage = NoteManager::instance()->storage(d->storageId());
    if (storage) {
        return storage->saveNote(*this);
    }
    qWarning("failed to save note. storage not found");
    return false;
}

bool Note::load()
{
    auto storage = this->storage();
    return storage && storage->loadNote(*this);
}

void Note::remove()
{
    auto storage = this->storage();
    if (storage)
        DraftManager::instance()->queueRemoval(storage->systemName(), id());
}

void Note::setTitle(const QString &title)
{
    if (!d)
        return;
    d.detach();
    d->title_ = title;
}

void Note::setText(const QString &text, Format format)
{
    if (!d)
        return;
    d.detach();
    d->setText(text, format);
}

void Note::setId(const QString &id)
{
    if (!d)
        return;
    d.detach();
    d->setId(id);
}

void Note::setFormat(Format format)
{
    if (!d)
        return;
    d.detach();
    d->format_ = format;
}

void Note::setTags(const QStringList &tags)
{
    if (!d)
        return;
    d.detach();
    d->setTags(tags);
}

void Note::setFolderId(const QUuid &folderId)
{
    if (!d)
        return;
    d.detach();
    d->folderId_ = folderId;
}

void Note::unload()
{
    if (!d)
        return;
    d.detach();
    d->unload();
}

void Note::setLastChangeUTC(const QDateTime &lastChange)
{
    if (!d)
        return;
    d.detach();
    d->lastChange_ = lastChange;
}

void Note::setBackendValue(const QString &key, const QVariant &value)
{
    if (!d)
        return;
    d.detach();
    d->setBackendValue(key, value);
}

void Note::removeBackendValue(const QString &key)
{
    if (!d)
        return;
    d.detach();
    d->removeBackendValue(key);
}

NoteStorage *Note::storage() const { return d ? d->storage_.data() : nullptr; }

QString Note::storageId() const { return d ? d->storageId() : QString(); }

QString Note::id() const { return d ? d->id_ : QString(); }

QString Note::text() const { return d ? d->text_ : QString(); }

QString Note::title() const { return d ? d->title_ : QString(); }

QString Note::displayTitle() const { return d ? displayTitleForText(d->title_, d->format_) : QString(); }

QString Note::displayTitleForText(const QString &title, Format format)
{
    if (format != Markdown)
        return title;

    const QString audioTitle = audioDisplayTitle(title);
    if (!audioTitle.isNull())
        return audioTitle;
    const QString imageTitle = htmlImageDisplayTitle(title);
    if (!imageTitle.isNull())
        return imageTitle;
    if (!title.contains(QLatin1Char('[')))
        return title;

    QTextDocument document;
    document.setMarkdown(title);
    QString result;
    for (auto fragment = document.begin().begin(); !fragment.atEnd(); ++fragment) {
        const auto textFragment = fragment.fragment();
        if (!textFragment.isValid())
            continue;
        const auto charFormat = textFragment.charFormat();
        if (!charFormat.isImageFormat()) {
            result += textFragment.text();
            continue;
        }
        const auto imageFormat = charFormat.toImageFormat();
        auto       label       = imageFormat.property(QTextFormat::ImageAltText).toString();
        if (label.isEmpty())
            label = imageFormat.property(QTextFormat::ImageTitle).toString();
        if (label.isEmpty())
            label = QFileInfo(QUrl(imageFormat.name()).path()).fileName();
        result += label;
    }
    return result.trimmed();
}

QStringList Note::tags() const { return d ? d->tags() : QStringList(); }

QUuid Note::folderId() const { return d ? d->folderId_ : QUuid {}; }

NoteData *Note::data() const { return d.data(); }

Note::Format Note::format() const { return d ? d->format_ : PlainText; }

QDateTime Note::lastChangeUTC() const { return d ? d->lastChange_ : QDateTime(); }

QVariant Note::backendValue(const QString &key) const { return d ? d->backendValue(key) : QVariant(); }

QVariantMap Note::backendData() const { return d ? d->backendData_ : QVariantMap {}; }

void Note::setBackendData(const QVariantMap &values)
{
    if (!d)
        return;
    d.detach();
    d->backendData_ = values;
}

QList<MediaReference> Note::media() const { return d ? d->media_ : QList<MediaReference> {}; }

void Note::setMedia(const QList<MediaReference> &media)
{
    if (!d)
        return;
    d.detach();
    d->media_ = media;
}

bool Note::isUpdated() const
{
    if (isNull() || id().isEmpty() || !storage()) {
        return true;
    }

    auto freshNote = storage()->note(id());
    return freshNote.isNull() || freshNote.lastChangeUTC() <= lastChangeUTC();
}

} // namespace AnyKeep
