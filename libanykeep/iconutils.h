#ifndef ICONUTILS_H
#define ICONUTILS_H

#include <QString>

#include "anykeep_export.h"

class QColor;
class QIcon;

namespace AnyKeep {

class ANYKEEP_EXPORT IconUtils {
public:
    static bool  isDarkColorScheme();
    static QIcon tintedIcon(const QIcon &icon, const QColor &color);
    static QIcon tintedSymbolicIcon(const QString &path, const QColor &color);
    static QIcon symbolicIcon(const QString &path);
    static QIcon themedIcon(const QString &name, const QString &fallbackPath);
    static QIcon themedIcon(const QString &name, const QString &fallbackPath, const QColor &color);
};

} // namespace AnyKeep

#endif // ICONUTILS_H
