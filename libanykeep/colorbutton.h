#ifndef ANYKEEP_COLORBUTTON_H
#define ANYKEEP_COLORBUTTON_H

#include <QWidget>

namespace AnyKeep {

class ColorButton : public QWidget {
    Q_OBJECT
public:
    explicit ColorButton(QWidget *parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags());

    void   setColor(QPalette::ColorRole role, const QColor &color);
    QColor color() const { return _color; }
signals:

public slots:

protected:
    void mousePressEvent(QMouseEvent *ev);
    void paintEvent(QPaintEvent *);

private:
    QPalette::ColorRole _role;
    QColor              _color;
};

} // namespace AnyKeep

#endif // ANYKEEP_COLORBUTTON_H
