/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import Meta from 'gi://Meta';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const RETRY_INTERVAL_SECONDS = 3;

export class Keybindings {
    constructor(settings, dbus) {
        this._settings = settings;
        this._dbus = dbus;
        this._enabled = false;
        this._registered = new Set();
        this._settingsSignalIds = new Map();
        this._retrySourceId = 0;
        this._dbus.onGlobalShortcutsChanged(() => this._loadIds());
    }

    enable() {
        if (this._enabled)
            return;

        this._enabled = true;
        this._loadIds();
    }

    disable() {
        if (!this._enabled)
            return;

        this._enabled = false;
        if (this._retrySourceId) {
            GLib.source_remove(this._retrySourceId);
            this._retrySourceId = 0;
        }
        this._clearRegistrations();
    }

    _loadIds() {
        if (!this._enabled)
            return;

        this._dbus.globalShortcutsJson()
            .then(response => {
                if (!this._enabled)
                    return;

                const shortcuts = JSON.parse(response);
                this._clearRegistrations();
                for (const shortcut of shortcuts)
                    this._register(shortcut.id, shortcut.accelerator);

                if (shortcuts.length === 0)
                    this._scheduleRetry();
            })
            .catch(error => {
                logError(error, 'Failed to load AnyKeep global shortcut ids');
                this._scheduleRetry();
            });
    }

    _scheduleRetry() {
        if (!this._enabled)
            return;

        if (this._retrySourceId)
            return;

        this._retrySourceId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, RETRY_INTERVAL_SECONDS, () => {
            this._retrySourceId = 0;
            this._loadIds();
            return GLib.SOURCE_REMOVE;
        });
    }

    _register(id, accelerator = null) {
        if (this._registered.has(id))
            return;

        try {
            if (accelerator !== null)
                this._settings.set_strv(id, accelerator.length > 0 ? [accelerator] : []);

            Main.wm.addKeybinding(
                id,
                this._settings,
                Meta.KeyBindingFlags.NONE,
                Shell.ActionMode.NORMAL | Shell.ActionMode.OVERVIEW,
                () => this._dbus.activateGlobalShortcut(id));
            this._registered.add(id);
            this._watch(id);
        } catch (error) {
            logError(error, `Failed to register AnyKeep global shortcut: ${id}`);
        }
    }

    _clearRegistrations() {
        for (const id of this._registered)
            Main.wm.removeKeybinding(id);
        this._registered.clear();
        for (const signalId of this._settingsSignalIds.values())
            this._settings.disconnect(signalId);
        this._settingsSignalIds.clear();
    }

    _watch(id) {
        if (this._settingsSignalIds.has(id))
            return;

        const signalId = this._settings.connect(`changed::${id}`, () => this._reregister(id));
        this._settingsSignalIds.set(id, signalId);
    }

    _reregister(id) {
        if (!this._enabled || !this._registered.has(id))
            return;

        try {
            Main.wm.removeKeybinding(id);
            this._registered.delete(id);
            this._register(id);
        } catch (error) {
            logError(error, `Failed to update AnyKeep global shortcut: ${id}`);
        }
    }
}
