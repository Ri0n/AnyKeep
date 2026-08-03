/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const ANYKEEP_APP_ID = 'anykeep.desktop';
const RETRY_INTERVAL_MS = 100;
const MAX_ATTEMPTS = 15;

export class AnyKeepWindowActivator {
    constructor() {
        this._sourceId = 0;
        this._attempt = 0;
    }

    destroy() {
        if (this._sourceId) {
            GLib.source_remove(this._sourceId);
            this._sourceId = 0;
        }
    }

    requestActivation() {
        this.destroy();
        this._attempt = 0;
        this._sourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RETRY_INTERVAL_MS, () => {
            this._attempt++;
            if (this._activatePendingWindow() || this._attempt >= MAX_ATTEMPTS) {
                this._sourceId = 0;
                return GLib.SOURCE_REMOVE;
            }
            return GLib.SOURCE_CONTINUE;
        });
    }

    _activatePendingWindow() {
        const windows = this._anykeepWindows();
        if (windows.length === 0)
            return false;

        let window = windows.find(w => w.demands_attention || w.urgent);
        if (!window && this._attempt >= 3)
            window = windows.sort((a, b) => this._userTime(b) - this._userTime(a))[0];
        if (!window)
            return false;

        try {
            Main.activateWindow(window, global.get_current_time());
            return true;
        } catch (error) {
            logError(error, 'Failed to activate AnyKeep window');
            return false;
        }
    }

    _anykeepWindows() {
        const app = Shell.AppSystem.get_default().lookup_app(ANYKEEP_APP_ID);
        if (app)
            return app.get_windows();

        return global.get_window_actors()
            .map(actor => actor.meta_window)
            .filter(window => {
                const wmClass = window.get_wm_class()?.toLowerCase();
                return wmClass === 'anykeep';
            });
    }

    _userTime(window) {
        return typeof window.get_user_time === 'function' ? window.get_user_time() : 0;
    }
}
