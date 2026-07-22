/*
QtNote - Simple note-taking application
Copyright (C) 2020 Sergei Ilinykh

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

#ifndef PLUGIN_HOST_INTERFACE_H
#define PLUGIN_HOST_INTERFACE_H

#include <QString>

#include <memory>

namespace QtNote {

class NoteManager;
class HighlighterExtension;
class SpellCheckProvider;

class PluginHostInterface {
public:
    virtual QString      utilsCuttedDots(const QString &str, int n) = 0;
    virtual NoteManager *noteManager()                              = 0;
    virtual QString      qtnoteDataDir()                            = 0;
    virtual void         rehighlight()                              = 0; // invalide syntax highligh for all open notes
    virtual bool         offerSpellCheckProvider(std::shared_ptr<SpellCheckProvider> provider) = 0;
};

}

#endif // PLUGIN_HOST_INTERFACE_H
