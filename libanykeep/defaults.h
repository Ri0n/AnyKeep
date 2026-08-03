#ifndef DEFAULTS_H
#define DEFAULTS_H

#include <QColor>

namespace AnyKeep {

class Defaults {
public:
    static QColor firstLineHighlightColor() { return QColor(255, 0, 0, 180); }
};

}

#endif // DEFAULTS_H
