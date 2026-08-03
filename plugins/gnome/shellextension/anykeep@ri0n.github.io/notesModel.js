/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import GLib from 'gi://GLib';

const PAGE_SIZE = 50;
const SEARCH_DELAY_MS = 250;

export class NotesModel {
    constructor(dbus) {
        this._dbus = dbus;
        this._callbacks = new Set();
        this._refreshSourceId = 0;
        this._requestSerial = 0;

        this.items = [];
        this.query = '';
        this.available = true;
        this.loading = false;
        this.loadingMore = false;
        this.hasMore = false;

        this._dbus.onNotesChanged(() => this.refresh());
    }

    destroy() {
        if (this._refreshSourceId) {
            GLib.Source.remove(this._refreshSourceId);
            this._refreshSourceId = 0;
        }
        this._callbacks.clear();
    }

    onChanged(callback) {
        this._callbacks.add(callback);
        return () => this._callbacks.delete(callback);
    }

    setQuery(query) {
        query = query.trim();
        if (this.query === query)
            return;

        this.query = query;
        this.loading = true;
        this.loadingMore = false;
        this._requestSerial++;
        this._notify();
        this._scheduleRefresh();
    }

    refresh() {
        if (this._refreshSourceId) {
            GLib.Source.remove(this._refreshSourceId);
            this._refreshSourceId = 0;
        }
        this._requestPage(0, false);
    }

    loadMore() {
        if (!this.hasMore || this.loading || this.loadingMore)
            return;

        this._requestPage(this.items.length, true);
    }

    _scheduleRefresh() {
        if (this._refreshSourceId)
            GLib.Source.remove(this._refreshSourceId);

        this._refreshSourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, SEARCH_DELAY_MS, () => {
            this._refreshSourceId = 0;
            this.refresh();
            return GLib.SOURCE_REMOVE;
        });
    }

    async _requestPage(offset, append) {
        if (append)
            this.loadingMore = true;
        else
            this.loading = true;
        this._notify();

        const requestSerial = ++this._requestSerial;
        try {
            const response = await this._dbus.notesJson(offset, PAGE_SIZE, this.query);
            if (requestSerial !== this._requestSerial)
                return;

            const parsed = JSON.parse(response);
            const notes = Array.isArray(parsed.notes) ? parsed.notes : [];

            this.available = true;
            this.hasMore = parsed.hasMore === true;
            if (append)
                this.items = this.items.concat(notes);
            else
                this.items = notes;
        } catch (error) {
            if (requestSerial !== this._requestSerial)
                return;

            const errorMessage = String(error);
            if (!errorMessage.includes('org.freedesktop.DBus.Error.UnknownObject')
                && !errorMessage.includes('org.freedesktop.DBus.Error.ServiceUnknown')) {
                logError(error, 'Failed to fetch AnyKeep notes');
            }
            this.available = false;
            this.hasMore = false;
            if (!append)
                this.items = [];
        } finally {
            if (requestSerial === this._requestSerial) {
                this.loading = false;
                this.loadingMore = false;
                this._notify();
            }
        }
    }

    _notify() {
        for (const callback of this._callbacks)
            callback();
    }
}
