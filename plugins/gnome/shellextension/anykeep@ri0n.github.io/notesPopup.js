/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import Clutter from 'gi://Clutter';
import St from 'gi://St';

import {gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

export class NotesPopup {
    constructor(menu, model, dbus) {
        this._menu = menu;
        this._model = model;
        this._dbus = dbus;
        this._signalIds = [];
        this._modelDisconnect = this._model.onChanged(() => this._render());

        this._box = new St.BoxLayout({
            vertical: true,
            style_class: 'anykeep-popup',
        });
        this._menu.box.add_child(this._box);

        this._actions = new St.BoxLayout({
            style_class: 'anykeep-actions',
        });
        this._box.add_child(this._actions);
        this._addAction(_('New Note'), '+', () => this._dbus.createNote());
        this._addAction(_('Note Manager'), 'view-list-symbolic', () => this._dbus.showNoteManager());
        this._addAction(_('Configure AnyKeep...'), 'preferences-system-symbolic', () => this._dbus.showOptions());
        this._addAction(_('About AnyKeep'), 'help-about-symbolic', () => this._dbus.showAbout());

        this._searchEntry = new St.Entry({
            hint_text: _('Search notes'),
            can_focus: true,
            track_hover: true,
            style_class: 'anykeep-search-entry',
        });
        this._box.add_child(this._searchEntry);
        this._signalIds.push([
            this._searchEntry.clutter_text,
            this._searchEntry.clutter_text.connect('text-changed', () => {
                this._model.setQuery(this._searchEntry.text);
            }),
        ]);

        this._scrollView = new St.ScrollView({
            hscrollbar_policy: St.PolicyType.NEVER,
            vscrollbar_policy: St.PolicyType.AUTOMATIC,
            overlay_scrollbars: true,
            style_class: 'anykeep-scroll-view',
        });
        this._list = new St.BoxLayout({
            vertical: true,
            style_class: 'anykeep-notes-list',
        });
        this._scrollView.set_child(this._list);
        this._box.add_child(this._scrollView);
    }

    destroy() {
        for (const [object, id] of this._signalIds)
            object.disconnect(id);
        this._signalIds = [];

        this._modelDisconnect?.();
        this._modelDisconnect = null;

        this._box?.destroy();
        this._box = null;
    }

    focusSearch() {
        this._searchEntry.grab_key_focus();
        this._searchEntry.clutter_text.grab_key_focus();
    }

    _addAction(label, iconName, callback) {
        const button = new St.Button({
            accessible_name: label,
            can_focus: true,
            style_class: 'button icon-button anykeep-action-button',
        });
        if (iconName === '+') {
            button.set_child(new St.Label({
                text: '+',
                x_align: Clutter.ActorAlign.CENTER,
                y_align: Clutter.ActorAlign.CENTER,
                style_class: 'anykeep-action-plus',
            }));
        } else {
            button.set_child(new St.Icon({
                icon_name: iconName,
                style_class: 'anykeep-action-icon',
            }));
        }
        button.connect('clicked', () => {
            callback();
            this._menu.close();
        });
        this._actions.add_child(button);
    }

    _render() {
        this._list.destroy_all_children();

        if (!this._model.available) {
            this._list.add_child(this._message(_('AnyKeep is not running')));
            return;
        }

        if (!this._model.loading && this._model.items.length === 0) {
            this._list.add_child(this._message(this._model.query ? _('No notes match the search') : _('No notes yet')));
            return;
        }

        for (const note of this._model.items)
            this._list.add_child(this._noteRow(note));

        if (this._model.loading || this._model.loadingMore)
            this._list.add_child(this._message(_('Loading...')));

        if (this._model.hasMore && !this._model.loading && !this._model.loadingMore)
            this._list.add_child(this._loadMoreRow());
    }

    _noteRow(note) {
        const row = new St.Button({
            can_focus: true,
            x_expand: true,
            style_class: 'anykeep-note-row',
        });
        const title = note.title && note.title.length > 0 ? note.title : _('Untitled Note');
        row.set_child(new St.Label({
            text: title,
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        }));
        row.connect('clicked', () => {
            this._dbus.openNote(note.storageId, note.id);
            this._menu.close();
        });
        return row;
    }

    _message(text) {
        return new St.Label({
            text,
            x_align: Clutter.ActorAlign.CENTER,
            style_class: 'anykeep-message',
        });
    }

    _loadMoreRow() {
        const row = new St.Button({
            can_focus: true,
            x_expand: true,
            style_class: 'anykeep-note-row anykeep-load-more-row',
        });
        row.set_child(new St.Label({
            text: _('Load more'),
            x_expand: true,
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.CENTER,
        }));
        row.connect('clicked', () => {
            this._model.loadMore();
        });
        return row;
    }
}
