/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {AnyKeepDBusClient} from './dbus.js';
import {AnyKeepIndicator} from './indicator.js';
import {Keybindings} from './keybindings.js';
import {StickyNotes} from './stickyNotes.js';
import {WindowGeometry} from './windowGeometry.js';

export default class AnyKeepExtension extends Extension {
    enable() {
        this._dbus = new AnyKeepDBusClient();
        this._settings = this.getSettings('org.gnome.shell.extensions.anykeep');
        this._keybindings = new Keybindings(this._settings, this._dbus);
        this._keybindings.enable();
        this._windowGeometry = new WindowGeometry(this._dbus);
        this._windowGeometry.enable();
        this._stickyNotes = new StickyNotes(this._settings, this._dbus);
        this._stickyNotes.enable();
        this._indicator = new AnyKeepIndicator(this, this._dbus);
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        this._stickyNotes?.disable();
        this._stickyNotes = null;

        this._windowGeometry?.disable();
        this._windowGeometry = null;

        this._indicator?.destroy();
        this._indicator = null;

        this._keybindings?.disable();
        this._keybindings = null;
        this._settings = null;

        this._dbus?.destroy();
        this._dbus = null;
    }
}
