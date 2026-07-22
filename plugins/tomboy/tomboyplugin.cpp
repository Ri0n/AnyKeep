/*
QtNote - Simple note-taking application
Copyright (C) 2015 Sergei Ilinykh

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

#include <QtPlugin>
#include <memory>

#include "notemanager.h"
#include "pluginhostinterface.h"
#include "qtnote_config.h"
#include "tomboyplugin.h"
#include "tomboystorage.h"

namespace QtNote {

static const QLatin1String pluginId("tomboy_storge");
static NoteStorage::Ptr    storage;

//------------------------------------------------------------
// TomboyPlugin
//------------------------------------------------------------
TomboyPlugin::TomboyPlugin(QObject *parent) : QObject(parent), host(nullptr) { }

TomboyPlugin::~TomboyPlugin() { shutdown(); }

int TomboyPlugin::metadataVersion() const { return MetadataVersion; }

void TomboyPlugin::setHost(PluginHostInterface *host) { this->host = host; }

PluginMetadata TomboyPlugin::metadata()
{
    PluginMetadata md;
    md.id          = pluginId;
    md.icon        = QIcon(":/icons/tomboy");
    md.name        = "Tomboy Storage";
    md.description = tr("Allows read and write tomboy notes");
    md.author      = "Sergei Ilinykh <rion4ik@gmail.com>";
    md.version     = 0x010000;       // plugin's version 0xXXYYZZPP
    md.minVersion  = 0x020300;       // minimum compatible version of QtNote
    md.maxVersion  = QTNOTE_VERSION; // maximum compatible version of QtNote
    md.homepage    = QUrl("http://ri0n.github.io/QtNote");
    return md;
}

bool TomboyPlugin::initialize()
{
    shutdown();
    if (!host || !host->noteManager())
        return false;
    auto ownedStorage = std::make_unique<TomboyStorage>(nullptr);
    storage           = ownedStorage.get();
    host->noteManager()->registerStorage(std::move(ownedStorage));
    return storage && storage->isAccessible();
}

void TomboyPlugin::shutdown()
{
    if (!storage)
        return;
    if (host && host->noteManager())
        host->noteManager()->unregisterStorage(storage.data());
    storage.clear();
}

} // namespace QtNote
