/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import Clutter from 'gi://Clutter';
import GObject from 'gi://GObject';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';

import {NotesModel} from './notesModel.js';
import {NotesPopup} from './notesPopup.js';

export const AnyKeepIndicator = GObject.registerClass(
class AnyKeepIndicator extends PanelMenu.Button {
    _init(extension, dbus) {
        super._init(0.0, 'AnyKeep');
        this.add_style_class_name('anykeep-panel-button');

        this._dbus = dbus;
        this._middleButtonPressed = false;
        this._focusSourceId = 0;
        this._model = new NotesModel(this._dbus);
        this._popup = new NotesPopup(this.menu, this._model, this._dbus);

        this.add_child(new St.Icon({
            icon_name: 'anykeep-symbolic',
            style_class: 'system-status-icon anykeep-panel-icon',
        }));

        this.connect('captured-event', this._onCapturedEvent.bind(this));
        this.menu.connect('open-state-changed', (menu, open) => {
            if (!open)
                return;

            this._model.refresh();
            this._scheduleSearchFocus();
        });
    }

    destroy() {
        if (this._focusSourceId) {
            GLib.source_remove(this._focusSourceId);
            this._focusSourceId = 0;
        }

        this._popup?.destroy();
        this._popup = null;

        this._model?.destroy();
        this._model = null;

        super.destroy();
    }

    _scheduleSearchFocus() {
        if (this._focusSourceId)
            GLib.source_remove(this._focusSourceId);

        this._focusSourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 50, () => {
            this._focusSourceId = 0;
            if (this.menu.isOpen)
                this._popup.focusSearch();
            return GLib.SOURCE_REMOVE;
        });
    }

    _onCapturedEvent(actor, event) {
        const eventType = event.type();
        if (eventType !== Clutter.EventType.BUTTON_PRESS && eventType !== Clutter.EventType.BUTTON_RELEASE)
            return Clutter.EVENT_PROPAGATE;

        if (event.get_button() !== 2)
            return Clutter.EVENT_PROPAGATE;

        switch (eventType) {
        case Clutter.EventType.BUTTON_PRESS:
            this._middleButtonPressed = true;
            this._dbus.createNote();
            this.menu.close();
            return Clutter.EVENT_STOP;
        case Clutter.EventType.BUTTON_RELEASE:
            if (this._middleButtonPressed) {
                this._middleButtonPressed = false;
                return Clutter.EVENT_STOP;
            }
            break;
        default:
            break;
        }

        return Clutter.EVENT_PROPAGATE;
    }
});
