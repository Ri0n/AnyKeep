/*
QtNote - Simple note-taking application
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

#include "utils.h"

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QXmlStreamWriter>

#include "qtnote_config.h"

namespace QtNote {

QString Utils::cuttedDots(const QString &src, int length)
{
    Q_ASSERT(length > 3);
    if (src.length() > length) {
        return src.left(length - 3) + "...";
    }
    return src;
}

const QString Utils::genericDataDir() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation); }

const QString &Utils::qtnoteDataDir()
{
    static QString dataDir;
    if (dataDir.isEmpty()) {
#ifdef Q_OS_ANDROID
        // Android application data must stay in the app-specific internal
        // directory. GenericDataLocation may resolve to shared storage.
        dataDir = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        if (!QDir().mkpath(dataDir))
            qWarning() << "Could not create QtNote data directory" << dataDir;
#else
        QSettings s;
        dataDir = genericDataDir() + QLatin1Char('/') + s.organizationName() + QLatin1Char('/') + s.applicationName();
#endif
    }
    return dataDir;
}

bool Utils::isAutostartEnabled()
{
#ifdef Q_OS_LINUX
    QFile desktop(QDir::homePath() + QStringLiteral("/.config/autostart/") + APPNAME + QStringLiteral(".desktop"));
    static const QRegularExpression enabledPattern(QStringLiteral("\\bhidden\\s*=\\s*false"),
                                                   QRegularExpression::CaseInsensitiveOption);
    return desktop.open(QIODevice::ReadOnly) && QString::fromUtf8(desktop.readAll()).contains(enabledPattern);
#elif defined(Q_OS_WIN)
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                       QSettings::NativeFormat);
    return registry.contains(QCoreApplication::applicationName());
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return QFile::exists(QDir::homePath() + QStringLiteral("/Library/LaunchAgents/com.github.ri0n.QtNote.plist"));
#else
    return false;
#endif
}

bool Utils::setAutostartEnabled(bool enabled)
{
#ifdef Q_OS_LINUX
    QDir home = QDir::home();
    if (!home.mkpath(QStringLiteral(".config/autostart")))
        return false;

    const QString destinationPath = home.absoluteFilePath(QStringLiteral(".config/autostart/" APPNAME ".desktop"));
    QByteArray    contents;
    QFile         existing(destinationPath);
    if (existing.open(QIODevice::ReadOnly)) {
        contents = existing.readAll();
    } else {
        const QString sourcePath = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                          QStringLiteral("applications/" APPNAME ".desktop"));
        QFile         source(sourcePath);
        if (sourcePath.isEmpty() || !source.open(QIODevice::ReadOnly))
            return false;
        contents = source.readAll();
    }

    static const QRegularExpression hiddenLine(QStringLiteral("^\\s*Hidden\\s*=.*(?:\\r?\\n|$)"),
                                               QRegularExpression::CaseInsensitiveOption
                                                   | QRegularExpression::MultilineOption);
    QString                         text = QString::fromUtf8(contents);
    text.remove(hiddenLine);
    if (!text.endsWith(QLatin1Char('\n')))
        text += QLatin1Char('\n');
    text += enabled ? QStringLiteral("Hidden=false\n") : QStringLiteral("Hidden=true\n");

    QSaveFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    if (destination.write(text.toUtf8()) < 0)
        return false;
    return destination.commit();
#elif defined(Q_OS_WIN)
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                       QSettings::NativeFormat);
    if (enabled)
        registry.setValue(QCoreApplication::applicationName(),
                          QLatin1Char('"') + QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
                              + QLatin1Char('"'));
    else
        registry.remove(QCoreApplication::applicationName());
    return registry.status() == QSettings::NoError;
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    QDir home = QDir::home();
    if (!home.mkpath(QStringLiteral("Library/LaunchAgents")))
        return false;

    const QString path = home.absoluteFilePath(QStringLiteral("Library/LaunchAgents/com.github.ri0n.QtNote.plist"));
    if (!enabled)
        return !QFile::exists(path) || QFile::remove(path);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeDTD(QStringLiteral("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"));
    xml.writeStartElement(QStringLiteral("plist"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
    xml.writeStartElement(QStringLiteral("dict"));
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("Label"));
    xml.writeTextElement(QStringLiteral("string"), QStringLiteral("com.github.ri0n.QtNote"));
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("ProgramArguments"));
    xml.writeStartElement(QStringLiteral("array"));
    xml.writeTextElement(QStringLiteral("string"), QCoreApplication::applicationFilePath());
    xml.writeEndElement();
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("RunAtLoad"));
    xml.writeEmptyElement(QStringLiteral("true"));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return !xml.hasError() && file.commit();
#else
    Q_UNUSED(enabled)
    return false;
#endif
}

QColor Utils::perceptiveColor(const QColor &against)
{
    float brightness = (0.299 * against.redF() + 0.587 * against.greenF() + 0.114 * against.blueF());
    if (brightness >= 0.5) {
        return QColor(Qt::black);
    }
    return QColor(Qt::white);
}

QColor Utils::mergeColors(const QColor &a, const QColor &b)
{
    qreal alphaA(a.alphaF());
    qreal alphaB(b.alphaF());
    return QColor::fromRgbF((a.redF() * alphaA) + (b.redF() * alphaB * (1 - alphaA)),
                            (a.greenF() * alphaA) + (b.greenF() * alphaB * (1 - alphaA)),
                            (a.blueF() * alphaA) + (b.blueF() * alphaB * (1 - alphaA)),
                            alphaA + (alphaB * (1 - alphaA)));
}

QString Utils::fileNameForText(const QDir &dir, const QString &text, const QString &fileExt, QString &sameBaseName)
{
    QString pfix = QLatin1Char('.') + fileExt;
    QString fileName;
    QString title = text.trimmed().split('\n')[0];
    if (title.isEmpty()) {
        return QString();
    }
    title = portableFileName(title, {}, 255 - pfix.size(), false);
    if (title.isEmpty())
        return {};

    if (title != sameBaseName) { // filename shoud be changed or it's new note
        QString suf;
        int     ind = 0;

        QString proposedId = title;
        while (dir.exists((fileName = dir.absoluteFilePath(QString("%1%2").arg(proposedId, pfix))))) {
            ind++;
            suf        = QString::number(ind);
            proposedId = title.left(255 - suf.size() - pfix.size()) + suf;
        }
        sameBaseName = proposedId;
    } else {
        fileName = dir.absoluteFilePath(QString("%1.%2").arg(title, fileExt));
    }
    return fileName;
}

QString Utils::portableFileName(const QString &source, const QString &fallback, qsizetype maxLength,
                                bool preserveSuffix)
{
    QString name = source.normalized(QString::NormalizationForm_C);
    name.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f<>:\"/\\\\|?*]")), QStringLiteral("_"));
    name.remove(QRegularExpression(QStringLiteral("[ .]+$")));
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        name = fallback;
    if (name.isEmpty())
        return {};

    static const QSet<QString> reserved { QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
                                          QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
                                          QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
                                          QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
                                          QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
                                          QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
                                          QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
                                          QStringLiteral("LPT9") };
    if (reserved.contains(QFileInfo(name).completeBaseName().toUpper()))
        name.prepend(QLatin1Char('_'));

    maxLength = qMax<qsizetype>(1, maxLength);
    if (name.size() > maxLength) {
        const auto      suffix       = preserveSuffix ? QFileInfo(name).suffix() : QString();
        const qsizetype suffixLength = suffix.isEmpty() ? 0 : suffix.size() + 1;
        if (suffixLength < maxLength)
            name = name.left(maxLength - suffixLength) + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
        else
            name = name.left(maxLength);
    }
    return name;
}

std::pair<QString, QString> Utils::splitTitle(const QString &text)
{
    auto    trimmed = text.trimmed();
    auto    idx     = trimmed.indexOf(QLatin1Char('\n'));
    QString title;
    QString body;
    if (idx == -1) {
        title = trimmed;
    } else {
        title = trimmed.left(idx);
        body  = trimmed.mid(idx + 1).trimmed();
    }
    return { title, body };
}

} // namespace QtNote
