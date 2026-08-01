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

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDialog>
#include <QLabel>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSettings>
#include <QVBoxLayout>

#include "defaults.h"
#include "filestorage.h"
#include "notemanager.h"
#include "optionsdlg.h"
#include "optionsplugins.h"
#include "pluginmanager.h"
#include "qtnote.h"
#include "qtnote_config.h"
#include "rulescontroller.h"
#include "settingswindow.h"
#include "shortcutedit.h"
#include "shortcutsmanager.h"
#include "storageiconimageprovider.h"
#include "storageprioritymodel.h"
#include "themediconimageprovider.h"
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
    priorityView  = new QQuickWidget(this);
    priorityView->setObjectName(QStringLiteral("animatedStoragePriorityView"));
    priorityView->setResizeMode(QQuickWidget::SizeRootObjectToView);
    priorityView->setClearColor(palette().color(QPalette::Base));
    installStorageIconImageProvider(priorityView->engine());
    installThemedIconImageProvider(priorityView->engine());
    priorityView->rootContext()->setContextProperty(QStringLiteral("settingsReorderModel"), priorityModel);
    priorityView->rootContext()->setContextProperty(QStringLiteral("settingsPluginMode"), false);
    priorityView->setSource(QUrl(QStringLiteral("qrc:/qml/AnimatedSettingsList.qml")));
    delete ui->verticalLayout_2->replaceWidget(ui->priorityView, priorityView);
    ui->priorityView->hide();
    if (priorityView->status() != QQuickWidget::Ready) {
        qWarning() << "Failed to create the animated storage priority list:" << priorityView->errors();
    } else {
        connect(priorityView->rootObject(), SIGNAL(configureRequested(QString)), this, SLOT(configureStorage(QString)));
    }

    auto *rulesPage   = new QWidget(this);
    auto *rulesLayout = new QVBoxLayout(rulesPage);
    rulesLayout->setContentsMargins(0, 0, 0, 0);
    rulesController = new RulesController(nullptr, nullptr, nullptr, rulesPage);
    rulesView       = new QQuickWidget(rulesPage);
    rulesView->setObjectName(QStringLiteral("rulesView"));
    rulesView->setResizeMode(QQuickWidget::SizeRootObjectToView);
    rulesView->setClearColor(palette().color(QPalette::Base));
    rulesView->rootContext()->setContextProperty(QStringLiteral("rulesController"), rulesController);
    rulesView->setSource(QUrl(QStringLiteral("qrc:/qml/RulesPage.qml")));
    rulesLayout->addWidget(rulesView);
    rulesPage->setMinimumSize(640, 390);
    ui->tabGeneral->addTab(rulesPage, tr("Rules"));
    if (rulesView->status() != QQuickWidget::Ready)
        qWarning() << "Failed to create the rules settings page:" << rulesView->errors();

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
        auto *field       = new QWidget(ui->gbShortcuts);
        auto *fieldLayout = new QVBoxLayout(field);
        fieldLayout->setContentsMargins(0, 0, 0, 0);
        fieldLayout->setSpacing(3);

        auto *se = new ShortcutEdit(qtnote, si.option, field);
        se->setObjectName("shortcut-" + si.option);
        se->setSequence(si.key);
        fieldLayout->addWidget(se);

        auto *error = new QLabel(field);
        error->setObjectName("shortcut-error-" + si.option);
        error->setWordWrap(true);
        error->setVisible(false);
        QPalette errorPalette = error->palette();
        errorPalette.setColor(QPalette::WindowText, QColor(190, 45, 35));
        error->setPalette(errorPalette);
        fieldLayout->addWidget(error);
        connect(se, &QLineEdit::textChanged, error, [se, error]() {
            error->clear();
            error->hide();
            se->setToolTip(QString());
            se->setStyleSheet(QString());
        });

        ((QFormLayout *)ui->gbShortcuts->layout())->addRow(si.name, field);
    }

    ui->plugins->layout()->addWidget(new OptionsPlugins(qtnote, this));

    adjustSize();
    resize(width(), height() + 100);
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
    if (rulesView && rulesView->rootObject()) {
        QVariant   saved;
        const bool invoked
            = QMetaObject::invokeMethod(rulesView->rootObject(), "saveCurrent", Q_RETURN_ARG(QVariant, saved));
        if (!invoked || !saved.toBool()) {
            ui->tabGeneral->setCurrentWidget(rulesView->parentWidget());
            return;
        }
    }

    QStringList storageCodes = priorityModel->priorityList();
    NoteManager::instance()->setPriorities(storageCodes);

    // const QMap<QString, QString> &shortcuts = qtnote->shortcutsManager()->all();
    foreach (ShortcutEdit *w, ui->gbShortcuts->findChildren<ShortcutEdit *>()) {
        if (!w->isModified()) {
            continue;
        }
        QString option = w->objectName().mid(sizeof("shortcut-") - 1);
        if (!qtnote->shortcutsManager()->setKey(option, w->sequence())) {
            QString message
                = tr("Failed to update shortcut for \"%1\"").arg(qtnote->shortcutsManager()->friendlyName(option));
            const QString detail = qtnote->shortcutsManager()->lastError();
            if (!detail.isEmpty())
                message = detail;

            auto *error = w->parentWidget()
                ? w->parentWidget()->findChild<QLabel *>(QStringLiteral("shortcut-error-") + option)
                : nullptr;
            if (error) {
                error->setText(message);
                error->show();
            }
            w->setToolTip(message);
            w->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid rgb(190, 45, 35); }"));
            ui->tabGeneral->setCurrentWidget(ui->shortcuts);
            w->setFocus(Qt::OtherFocusReason);
            w->selectAll();
            qtnote->shortcutsManager()->setShortcutEnable(option, true);
            return;
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

void OptionsDlg::configureStorage(const QString &storageId)
{
    const auto storage = NoteManager::instance()->storage(storageId);
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
