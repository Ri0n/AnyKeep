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

#ifndef OPTIONSDLG_H
#define OPTIONSDLG_H

#include <QDialog>
#include <QFont>
#include <QMap>

namespace Ui {
class OptionsDlg;
}

class QQuickWidget;

namespace QtNote {

class Main;
class StoragePriorityModel;

class OptionsDlg : public QDialog {
    Q_OBJECT
public:
    OptionsDlg(Main *qtnote = 0);
    ~OptionsDlg();

protected:
    void changeEvent(QEvent *e);

public slots:
    void accept();

private slots:
    void configureStorage(const QString &storageId);
    void on_pbDefaultFontAdv_clicked();

private:
    Ui::OptionsDlg       *ui;
    Main                 *qtnote;
    StoragePriorityModel *priorityModel;
    QQuickWidget         *priorityView;
    QFont                 defaultFont;
};

} // namespace QtNote

#endif // OPTIONSDLG_H
