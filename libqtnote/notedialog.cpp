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

#include <QApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QDesktopWidget>
#endif
#include <QHBoxLayout>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>

#include "deintegrationinterface.h"
#include "notedialog.h"
#include "notestorage.h"
#include "notewidget.h"
#include "qtnote.h"
#include "ui_notedialog.h"
#include "utils.h"

namespace QtNote {

static QString geometryKey(const NoteWidget *widget)
{
    const auto note = widget->note();
    if (!note.id().isEmpty())
        return QString("geometry.%1.%2").arg(note.storageId(), note.id());
    return QString("geometry.draft.%1").arg(widget->draftId().toString(QUuid::WithoutBraces));
}

NoteDialog::NoteDialog(NoteWidget *noteWidget, Main *main) :
    QDialog(0), m_ui(new Ui::NoteDialog), noteWidget(noteWidget), main(main),
    windowGeometryKey(geometryKey(noteWidget)), alwaysOnTopKey(windowGeometryKey + QStringLiteral(".always-on-top"))
{
    auto flags = (windowFlags() ^ (Qt::Dialog | Qt::WindowContextHelpButtonHint)) | Qt::WindowSystemMenuHint
        | Qt::CustomizeWindowHint | Qt::Window | Qt::WindowMinMaxButtonsHint;
    const bool alwaysOnTop = QSettings().value(alwaysOnTopKey, false).toBool();
    if (alwaysOnTop)
        flags |= Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);
    m_ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("noteDlg");

    QHBoxLayout *l = new QHBoxLayout;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    l->setMargin(2);
#else
    l->setContentsMargins(2, 2, 2, 2);
#endif
    l->addWidget(noteWidget);
    setLayout(l);

    QRect rect;
    bool  geometryHandled = false;
    {
        auto       note          = noteWidget->note();
        const auto restoreResult = main->restoreWindowGeometry(this, windowGeometryKey);
        geometryHandled          = restoreResult != WindowGeometryRestoreResult::Unsupported;
        rect                     = QSettings().value(windowGeometryKey).toRect();
        if (restoreResult == WindowGeometryRestoreResult::Pending) {
            // Wayland applications cannot choose their global position, but they
            // still own their initial size while KWin restores frameGeometry.
            if (rect.isValid())
                resize(rect.size());
            else
                resize(sizeHint().expandedTo(QSize(320, 240)));
            rect = {};
        } else if (!geometryHandled) {
            if (!rect.isValid() || !screen()->geometry().contains(rect)) {
                rect = {};
            }
        }

        if (!note.id().isEmpty()) {
            Q_ASSERT(!NoteDialog::findDialog(note.storageId(), note.id()));
            NoteDialog::dialogs.insert(QPair<QString, QString>(note.storageId(), note.id()), this);
        }
    }
    if (rect.isEmpty() && !geometryHandled) {
        QSize avail = screen()->availableSize() - sizeHint(); //   QApplication::desktop()->size()
        int   x     = QRandomGenerator::global()->bounded(avail.width() / 4, avail.width() / 2);
        int   y     = QRandomGenerator::global()->bounded(avail.height() / 4, avail.height() / 2);
        move(x, y);
    } else if (!rect.isEmpty()) {
        setGeometry(rect);
    }

    noteWidget->setAlwaysOnTop(alwaysOnTop);
    noteWidget->setFocus(Qt::OtherFocusReason);

    connect(noteWidget, &NoteWidget::trashRequested, this, &NoteDialog::trashRequested);
    connect(noteWidget, &NoteWidget::alwaysOnTopChanged, this, [this](bool enabled) {
        QSettings().setValue(alwaysOnTopKey, enabled);
        const bool active = windowFlags().testFlag(Qt::WindowStaysOnTopHint);
        if (active == enabled)
            return;
        setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
        show();
        raise();
        activateWindow();
    });
    connect(noteWidget, &NoteWidget::pinRequested, this, [this]() {
        pinning = true;
        close();
    });
    connect(noteWidget, &NoteWidget::firstLineChanged, this, &NoteDialog::firstLineChanged);

    firstLineChanged();
}

NoteDialog::~NoteDialog() { delete m_ui; }

NoteDialog *NoteDialog::findDialog(const QString &storageId, const QString &noteId)
{
    return NoteDialog::dialogs.value(QPair<QString, QString>(storageId, noteId));
}

QList<NoteDialog *> NoteDialog::openDialogs() { return NoteDialog::dialogs.values(); }

void NoteDialog::registerWindowGeometry() { main->restoreWindowGeometry(this, windowGeometryKey); }

void NoteDialog::changeEvent(QEvent *e)
{
    switch (e->type()) {
    case QEvent::LanguageChange:
        m_ui->retranslateUi(this);
        break;
    default:
        break;
    }
    QDialog::changeEvent(e);
}

void NoteDialog::trashRequested()
{
    noteWidget->setTrashRequested(true); // in case when called outside
    noteWidget->discardDraft();
    close();
}

void NoteDialog::done(int r)
{
    noteWidget->disconnect(this);
    if (!noteWidget->isTrashRequested() && !noteWidget->prepareToClose()) {
        return;
    }
    QSettings s;
    if (noteWidget->isTrashRequested()) {
        main->removeWindowGeometry(windowGeometryKey);
        s.remove(windowGeometryKey);
        s.remove(alwaysOnTopKey);
    } else {
        if (!main->saveWindowGeometry(this, windowGeometryKey))
            s.setValue(windowGeometryKey, geometry());
    }
    if (!noteWidget->note().id().isEmpty()) {
        NoteDialog::dialogs.remove(
            QPair<QString, QString>(noteWidget->note().storage()->systemName(), noteWidget->note().id()));
    }
    if (pinning) {
        main->pinNote(noteWidget->note(), noteWidget->draftId(), noteWidget->hasPersistedDraft(), frameGeometry());
    }
    QDialog::done(r);
}

void NoteDialog::firstLineChanged()
{
    QString l = noteWidget->firstLine().trimmed();
    setWindowTitle(Utils::cuttedDots(l.isEmpty() ? tr("[No Title]") : l, 256));
}

QHash<QPair<QString, QString>, NoteDialog *> NoteDialog::dialogs;

} // namespace QtNote
