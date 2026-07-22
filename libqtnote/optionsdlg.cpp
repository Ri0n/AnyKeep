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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDialog>
#include <QSettings>

#include "defaults.h"
#include "filestorage.h"
#include "notemanager.h"
#include "optionsdlg.h"
#include "optionsplugins.h"
#include "pluginmanager.h"
#include "qtnote.h"
#include "qtnote_config.h"
#include "settingswindow.h"
#include "shortcutedit.h"
#include "shortcutsmanager.h"
#include "storageprioritymodel.h"
#include "ui_optionsdlg.h"
#include "utils.h"

namespace QtNote {

OptionsDlg::OptionsDlg(Main *qtnote) : QDialog(0), ui(new Ui::OptionsDlg), qtnote(qtnote)
{
    ui->setupUi(this);

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    ui->ckAutostart->setChecked(Utils::isAutostartEnabled());
#else
    ui->ckAutostart->setVisible(false);
#endif
    priorityModel = new StoragePriorityModel(this);
    ui->priorityView->setModel(priorityModel);
    QSettings s;
    ui->ckAskDel->setChecked(s.value("ui.ask-on-delete", true).toBool());
    ui->spMenuNotesAmount->setValue(s.value("ui.menu-notes-amount", 15).toInt());
    ui->wTitleColor->setColor(QPalette::Text,
                              s.value("ui.title-color", Defaults::firstLineHighlightColor()).value<QColor>());
    if (!defaultFont.fromString(s.value("ui.default-font").toString())) {
        defaultFont = this->font();
    }
    ui->fcbDefaultFont->setCurrentFont(defaultFont);
    auto dfPointSize = defaultFont.pointSizeF();
    if (dfPointSize == -1) {
        ui->fsbDefaultFontSize->setDecimals(0);
        ui->fsbDefaultFontSize->setSuffix(QLatin1String(" px"));
        ui->fsbDefaultFontSize->setValue(defaultFont.pixelSize());
    } else {
        ui->fsbDefaultFontSize->setDecimals(1);
        ui->fsbDefaultFontSize->setSuffix(QLatin1String(" pt"));
        ui->fsbDefaultFontSize->setValue(dfPointSize);
    }
    connect(ui->fcbDefaultFont, &QFontComboBox::currentFontChanged, this,
            [this](const QFont &font) { defaultFont = font; });
    connect(ui->fsbDefaultFontSize, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (defaultFont.pixelSize() == -1) {
            defaultFont.setPointSizeF(value);
        } else {
            defaultFont.setPixelSize(value);
        }
    });

    foreach (const ShortcutsManager::ShortcutInfo &si, qtnote->shortcutsManager()->all()) {
        ShortcutEdit *se = new ShortcutEdit(qtnote, si.option);
        se->setObjectName("shortcut-" + si.option);
        se->setSequence(si.key);
        ((QFormLayout *)ui->gbShortcuts->layout())->addRow(si.name, se);
    }

    ui->plugins->layout()->addWidget(new OptionsPlugins(qtnote, this));

    resize(0, 0);

    connect(ui->priorityView, &QListView::doubleClicked, this, &OptionsDlg::storage_doubleClicked);
}

OptionsDlg::~OptionsDlg() { delete ui; }

void OptionsDlg::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}

void OptionsDlg::accept()
{
    QStringList storageCodes = priorityModel->priorityList();
    NoteManager::instance()->setPriorities(storageCodes);

    // const QMap<QString, QString> &shortcuts = qtnote->shortcutsManager()->all();
    foreach (ShortcutEdit *w, ui->gbShortcuts->findChildren<ShortcutEdit *>()) {
        if (!w->isModified()) {
            continue;
        }
        QString option = w->objectName().mid(sizeof("shortcut-") - 1);
        if (!qtnote->shortcutsManager()->setKey(option, w->sequence())) {
            qtnote->notifyError(
                tr("Failed to update shortcut for \"%1\"").arg(qtnote->shortcutsManager()->friendlyName(option)));
        }
        qtnote->shortcutsManager()->setShortcutEnable(option, true);
    }

    QSettings s;
    s.setValue("ui.ask-on-delete", ui->ckAskDel->isChecked());
    s.setValue("ui.menu-notes-amount", ui->spMenuNotesAmount->value());
    s.setValue("ui.title-color", ui->wTitleColor->color());
    s.setValue("ui.default-font", defaultFont.toString());

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    Utils::setAutostartEnabled(ui->ckAutostart->isChecked());
#endif
    QDialog::accept();
}

void OptionsDlg::storage_doubleClicked(const QModelIndex &index)
{
    const QString storageId = priorityModel->storageId(index);
    const auto    storage   = NoteManager::instance()->storage(storageId);
    if (!storage)
        return;

    if (auto *controller = storage->createSettingsController(nullptr)) {
        auto component = storage->settingsComponent();
        if (component.isEmpty())
            component = QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml"));
        auto *window
            = new SettingsWindow(controller, component, storage->name() + QStringLiteral(": ") + tr("Settings"), this);
        connect(window, &SettingsWindow::applied, this, [this]() { emit qtnote->settingsUpdated(); });
        window->show();
        return;
    }
}

void OptionsDlg::on_pbDefaultFontAdv_clicked()
{
    bool  ok;
    QFont font = QFontDialog::getFont(&ok, defaultFont, this);
    if (ok) {
        defaultFont = font;
        ui->fcbDefaultFont->setCurrentFont(font);
        ui->fsbDefaultFontSize->setValue(font.pixelSize() == -1 ? font.pointSizeF() : font.pixelSize());
    }
}

} // namespace QtNote
