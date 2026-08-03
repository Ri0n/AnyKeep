/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

const ANYKEEP_APP_ID = 'anykeep.desktop';
const SAVE_DELAY_MS = 150;
const RETRY_INTERVAL_MS = 100;
const MAX_WATCH_ATTEMPTS = 20;
const MAX_CLAIM_ATTEMPTS = 20;

export class WindowGeometry {
    constructor(dbus) {
        this._dbus = dbus;
        this._displaySignalId = 0;
        this._windows = new Map();
        this._claiming = new Set();
        this._pendingWindows = new Map();
        this._anykeepPid = 0;
    }

    enable() {
        this._anykeepPid = this._dbus.processId();
        this._dbus.onNameOwnerChanged(owner => {
            this._anykeepPid = owner ? this._dbus.processId() : 0;
            if (owner)
                this._dbus.windowGeometryScriptReady();
        });
        this._displaySignalId = global.display.connect('window-created', (_display, window) => {
            this._watch(window);
        });
        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);
        this._dbus.windowGeometryScriptReady();
    }

    disable() {
        if (this._displaySignalId) {
            global.display.disconnect(this._displaySignalId);
            this._displaySignalId = 0;
        }
        for (const [window, state] of this._windows)
            this._disconnectWindow(window, state);
        for (const sourceId of this._pendingWindows.values())
            GLib.source_remove(sourceId);
        this._windows.clear();
        this._claiming.clear();
        this._pendingWindows.clear();
    }

    _isAnyKeepWindow(window) {
        if (!window)
            return false;

        if (!this._anykeepPid)
            this._anykeepPid = this._dbus.processId();
        if (this._anykeepPid && window.get_pid() === this._anykeepPid)
            return true;

        const app = Shell.AppSystem.get_default().lookup_app(ANYKEEP_APP_ID);
        if (app?.get_windows().includes(window))
            return true;

        return window.get_wm_class()?.toLowerCase() === 'anykeep';
    }

    _watch(window) {
        if (!window || this._windows.has(window) || this._pendingWindows.has(window))
            return;

        let attempt = 0;
        const tryWatch = () => {
            attempt++;
            if (this._isAnyKeepWindow(window)) {
                this._pendingWindows.delete(window);
                this._startWatching(window);
                return GLib.SOURCE_REMOVE;
            }
            if (attempt >= MAX_WATCH_ATTEMPTS) {
                this._pendingWindows.delete(window);
                const app = Shell.WindowTracker.get_default().get_window_app(window);
                console.log(
                    `AnyKeep geometry ignored window: pid=${window.get_pid()} ` +
                    `anykeepPid=${this._anykeepPid} type=${window.get_window_type()} ` +
                    `wmClass=${window.get_wm_class() ?? ''} app=${app?.get_id() ?? ''}`);
                return GLib.SOURCE_REMOVE;
            }
            return GLib.SOURCE_CONTINUE;
        };

        if (tryWatch() === GLib.SOURCE_REMOVE)
            return;
        const sourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RETRY_INTERVAL_MS, tryWatch);
        this._pendingWindows.set(window, sourceId);
    }

    _startWatching(window) {
        if (this._windows.has(window))
            return;
        const state = {
            key: '',
            claimAttempt: 0,
            claimSourceId: 0,
            saveSourceId: 0,
            signalIds: [],
        };
        state.signalIds.push(window.connect('position-changed', () => this._geometryChanged(window)));
        state.signalIds.push(window.connect('size-changed', () => this._geometryChanged(window)));
        state.signalIds.push(window.connect('unmanaged', () => this._windowUnmanaged(window)));
        this._windows.set(window, state);
        this._claim(window);
    }

    async _claim(window) {
        const state = this._windows.get(window);
        if (!state || state.key || this._claiming.has(window))
            return;

        this._claiming.add(window);
        state.claimAttempt++;
        try {
            const response = await this._dbus.claimWindowGeometry();
            const currentState = this._windows.get(window);
            if (!currentState)
                return;
            if (!response) {
                this._retryClaim(window, currentState);
                return;
            }

            const geometry = JSON.parse(response);
            if (!geometry.key)
                return;
            currentState.key = geometry.key;
            if (geometry.valid) {
                window.move_resize_frame(
                    false,
                    geometry.x,
                    geometry.y,
                    geometry.width,
                    geometry.height);
            }
        } catch (error) {
            logError(error, 'Failed to claim AnyKeep window geometry');
        } finally {
            this._claiming.delete(window);
        }
    }

    _retryClaim(window, state) {
        if (state.claimSourceId || state.claimAttempt >= MAX_CLAIM_ATTEMPTS)
            return;
        state.claimSourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RETRY_INTERVAL_MS, () => {
            state.claimSourceId = 0;
            this._claim(window);
            return GLib.SOURCE_REMOVE;
        });
    }

    _geometryChanged(window) {
        const state = this._windows.get(window);
        if (!state)
            return;
        if (!state.key) {
            this._claim(window);
            return;
        }
        if (state.saveSourceId)
            GLib.source_remove(state.saveSourceId);
        state.saveSourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, SAVE_DELAY_MS, () => {
            state.saveSourceId = 0;
            this._save(window, state);
            return GLib.SOURCE_REMOVE;
        });
    }

    _save(window, state) {
        if (!state.key)
            return;
        const rect = window.get_frame_rect();
        this._dbus.storeWindowGeometry(state.key, rect.x, rect.y, rect.width, rect.height);
    }

    _windowUnmanaged(window) {
        const pendingSourceId = this._pendingWindows.get(window);
        if (pendingSourceId) {
            GLib.source_remove(pendingSourceId);
            this._pendingWindows.delete(window);
        }
        const state = this._windows.get(window);
        if (!state)
            return;
        if (state.saveSourceId) {
            GLib.source_remove(state.saveSourceId);
            state.saveSourceId = 0;
        }
        if (state.claimSourceId) {
            GLib.source_remove(state.claimSourceId);
            state.claimSourceId = 0;
        }
        const rect = window.get_frame_rect();
        if (state.key)
            this._dbus.storeWindowGeometry(state.key, rect.x, rect.y, rect.width, rect.height);
        else
            this._saveUnclaimed(rect);
        this._disconnectWindow(window, state);
        this._windows.delete(window);
        this._claiming.delete(window);
    }

    async _saveUnclaimed(rect) {
        try {
            const response = await this._dbus.claimWindowGeometry();
            if (!response)
                return;
            const geometry = JSON.parse(response);
            if (geometry.key)
                this._dbus.storeWindowGeometry(geometry.key, rect.x, rect.y, rect.width, rect.height);
        } catch (error) {
            logError(error, 'Failed to save closing AnyKeep window geometry');
        }
    }

    _disconnectWindow(window, state) {
        if (state.claimSourceId)
            GLib.source_remove(state.claimSourceId);
        if (state.saveSourceId)
            GLib.source_remove(state.saveSourceId);
        for (const signalId of state.signalIds) {
            try {
                window.disconnect(signalId);
            } catch (_error) {
                // The Meta.Window may already be unmanaged.
            }
        }
    }
}
