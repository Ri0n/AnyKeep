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

#include <QKeyCombination>
#include <QKeyEvent>

#include "qtnote.h"
#include "shortcutedit.h"
#include "shortcutsmanager.h"

namespace QtNote {

ShortcutEdit::ShortcutEdit(Main *qtnote, const QString &option, QWidget *parent) :
    QLineEdit(parent), qtnote(qtnote), option(option)
{
}

void ShortcutEdit::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        setText("");
        _seq = QKeySequence();
        setModified(true);
        event->accept();
        return;
    }

    // Modifier key presses arrive as ordinary key events as well.  Building a
    // sequence from them produced strings such as "Alt+Alt" and could leave a
    // key-only combination after parsing.  Wait for the actual non-modifier
    // key; its event already contains all currently held modifiers.
    switch (event->key()) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
        event->accept();
        return;
    default:
        break;
    }
    if (event->key() == Qt::Key_unknown) {
        event->ignore();
        return;
    }

    const auto modifiers
        = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    const QKeySequence sequence(QKeyCombination(modifiers, Qt::Key(event->key())));
    const bool         changed = sequence != _seq;
    setSequence(sequence);
    if (changed)
        setModified(true);
    event->accept();
}

void ShortcutEdit::focusInEvent(QFocusEvent *ev)
{
    qtnote->shortcutsManager()->setShortcutEnable(option, false);
    QLineEdit::focusInEvent(ev);
}

void ShortcutEdit::focusOutEvent(QFocusEvent *ev)
{
    qtnote->shortcutsManager()->setShortcutEnable(option, true);
    QLineEdit::focusOutEvent(ev);
}

}
