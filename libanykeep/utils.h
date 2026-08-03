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

#ifndef UTILS_H
#define UTILS_H

#include <QString>

#include "anykeep_export.h"

class QColor;
class QDir;

namespace AnyKeep {

class ANYKEEP_EXPORT Utils {
public:
    static QString cuttedDots(const QString &, int);

    static const QString  genericDataDir();
    static const QString &anykeepDataDir();

    static bool isAutostartEnabled();
    static bool setAutostartEnabled(bool enabled);

    static QColor perceptiveColor(const QColor &against);
    static QColor mergeColors(const QColor &a, const QColor &b);

    /** Converts one filename component to a cross-platform portable form. */
    static QString portableFileName(const QString &name, const QString &fallback = {}, qsizetype maxLength = 180,
                                    bool preserveSuffix = true);

    /**
     * @brief fileNameForText converts text to an unique filename with the given extension.
     *     If base part of the converted filename matches with `sameBaseName` or the file doesn't exists in the given
     *     directory then the search for unique stops and the filename returned.
     * @param dir           - directory to have unique filename in.
     * @param text          - text to convert to a filename
     * @param fileExt       - extension for the filename
     * @param sameBaseName  - allowed name to ingore uniqueness (usually the original file we are going to rename)
     *                        if a new unique base is proposed it will be written back to sameBaseName
     * @return absolute path to the file
     */
    static QString fileNameForText(const QDir &dir, const QString &text, const QString &fileExt, QString &sameBaseName);

    static std::pair<QString, QString> splitTitle(const QString &text);
};

} // namespace AnyKeep

#endif // UTILS_H
