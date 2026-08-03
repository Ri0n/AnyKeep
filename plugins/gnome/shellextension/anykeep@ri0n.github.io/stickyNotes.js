/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import Clutter from 'gi://Clutter';
import St from 'gi://St';

const MIN_WIDTH = 220;
const MIN_HEIGHT = 140;

class StickyNoteActor {
    constructor(id, geometry, dbus, saveGeometry, dismiss) {
        this.id = id;
        this._dbus = dbus;
        this._saveGeometry = saveGeometry;
        this._dismiss = dismiss;

        this.actor = new St.Widget({
            reactive: true,
            style_class: 'anykeep-sticky',
            layout_manager: new Clutter.FixedLayout(),
        });
        this.actor.set_position(geometry.x, geometry.y);
        this.actor.set_size(Math.max(MIN_WIDTH, geometry.width), Math.max(MIN_HEIGHT, geometry.height));

        const content = new St.BoxLayout({
            vertical: true,
            x_expand: true,
            y_expand: true,
            style_class: 'anykeep-sticky-content',
        });
        this._title = new St.Label({
            x_expand: true,
            style_class: 'anykeep-sticky-title',
            text: '[No Title]',
        });
        this._title.clutter_text.set_ellipsize(3);
        const closeButton = new St.Button({
            label: '×',
            style_class: 'anykeep-sticky-close',
            can_focus: true,
        });
        closeButton.connect('clicked', () => {
            this._closeButtonPressed = false;
            this._dismiss(this.id);
        });
        closeButton.connect('button-press-event', () => {
            this._closeButtonPressed = true;
            return Clutter.EVENT_PROPAGATE;
        });
        closeButton.connect('button-release-event', () => {
            this._closeButtonPressed = false;
            return Clutter.EVENT_PROPAGATE;
        });
        const header = new St.BoxLayout({x_expand: true, style_class: 'anykeep-sticky-header'});
        header.add_child(this._title);
        header.add_child(closeButton);
        content.add_child(header);

        this._body = new St.Label({
            x_expand: true,
            y_expand: true,
            y_align: Clutter.ActorAlign.START,
            style_class: 'anykeep-sticky-body',
        });
        this._body.clutter_text.set_line_wrap(true);
        this._body.clutter_text.set_line_wrap_mode(2);
        this._body.clutter_text.set_ellipsize(3);
        content.add_child(this._body);
        this.actor.add_child(content);

        const grip = new St.DrawingArea({
            reactive: true,
            x_align: Clutter.ActorAlign.END,
            y_align: Clutter.ActorAlign.END,
            style_class: 'anykeep-sticky-resize-grip',
        });
        grip.set_size(18, 18);
        grip.connect('repaint', area => this._paintResizeGrip(area));
        this.actor.add_child(grip);
        const updateLayout = () => {
            content.set_size(this.actor.width, this.actor.height);
            grip.set_position(
                Math.max(0, this.actor.width - grip.width),
                Math.max(0, this.actor.height - grip.height));
        };
        this.actor.connect('notify::width', updateLayout);
        this.actor.connect('notify::height', updateLayout);
        updateLayout();

        grip.connect('button-press-event', (_actor, event) => {
            this._beginPointerOperation(event, true);
            return Clutter.EVENT_STOP;
        });
        this.actor.connect('button-press-event', (_actor, event) => {
            if (event.get_button() !== Clutter.BUTTON_PRIMARY)
                return Clutter.EVENT_PROPAGATE;
            if (this._closeButtonPressed)
                return Clutter.EVENT_PROPAGATE;
            const source = event.get_source();
            if (source && (source === closeButton || closeButton.contains(source)))
                return Clutter.EVENT_PROPAGATE;
            this._beginPointerOperation(event, false);
            return Clutter.EVENT_STOP;
        });
    }

    async refresh() {
        try {
            const payload = JSON.parse(await this._dbus.stickyNoteJson(this.id) || '{}');
            this._title.text = payload.title || '[No Title]';
            this._body.text = payload.body || '';
        } catch (error) {
            logError(error, `Failed to refresh AnyKeep sticky note ${this.id}`);
            this._body.text = 'AnyKeep is not running';
        }
    }

    destroy() {
        this._endPointerOperation(false);
        this.actor.destroy();
    }

    _paintResizeGrip(area) {
        const cr = area.get_context();
        const [width, height] = area.get_surface_size();
        const color = area.get_theme_node().get_foreground_color();
        cr.setSourceRGBA(color.red / 255, color.green / 255, color.blue / 255, color.alpha / 255);
        cr.setLineWidth(1.5);
        for (const radius of [5, 9, 13]) {
            cr.arc(width, height, radius, Math.PI, Math.PI * 1.5);
            cr.stroke();
        }
        cr.$dispose();
    }

    _beginPointerOperation(event, resize) {
        if (event.get_button() !== Clutter.BUTTON_PRIMARY)
            return;
        this._endPointerOperation(false);
        [this._pointerX, this._pointerY] = event.get_coords();
        [this._startX, this._startY] = this.actor.get_position();
        [this._startWidth, this._startHeight] = this.actor.get_size();
        this._resizing = resize;
        this._pointerMoved = false;
        this._capturedEventId = global.stage.connect('captured-event', (_stage, capturedEvent) => {
            const type = capturedEvent.type();
            if (type === Clutter.EventType.MOTION) {
                const [x, y] = capturedEvent.get_coords();
                const dx = x - this._pointerX;
                const dy = y - this._pointerY;
                if (Math.abs(dx) > 4 || Math.abs(dy) > 4)
                    this._pointerMoved = true;
                if (this._resizing) {
                    this.actor.set_size(
                        Math.max(MIN_WIDTH, Math.round(this._startWidth + dx)),
                        Math.max(MIN_HEIGHT, Math.round(this._startHeight + dy)));
                } else {
                    this.actor.set_position(
                        Math.round(this._startX + dx), Math.round(this._startY + dy));
                }
                return Clutter.EVENT_STOP;
            }
            if (type === Clutter.EventType.BUTTON_RELEASE &&
                capturedEvent.get_button() === Clutter.BUTTON_PRIMARY) {
                this._endPointerOperation(true);
                return Clutter.EVENT_STOP;
            }
            return Clutter.EVENT_PROPAGATE;
        });
    }

    _endPointerOperation(save) {
        if (!this._capturedEventId)
            return;
        global.stage.disconnect(this._capturedEventId);
        this._capturedEventId = 0;
        if (save) {
            this._save();
            if (!this._resizing && !this._pointerMoved) {
                const now = Date.now();
                if (now - (this._lastClickTime ?? 0) < 400) {
                    this._lastClickTime = 0;
                    this._dbus.openStickyNote(this.id);
                } else {
                    this._lastClickTime = now;
                }
            }
        }
    }

    _save() {
        const [x, y] = this.actor.get_position();
        const [width, height] = this.actor.get_size();
        this._saveGeometry(this.id, {x, y, width, height});
    }
}

export class StickyNotes {
    constructor(settings, dbus) {
        this._settings = settings;
        this._dbus = dbus;
        this._actors = new Map();
        this._settingsSignalId = 0;
        this._restackedSignalId = 0;
    }

    enable() {
        this._settingsSignalId = this._settings.connect('changed::sticky-notes', () => this._sync());
        this._dbus.onStickyNotesChanged(() => this._refreshAll());
        this._dbus.onNameOwnerChanged(owner => {
            if (owner)
                this._refreshAll();
        });
        this._restackedSignalId = global.display.connect('restacked', () => this._lowerActors());
        this._sync();
    }

    disable() {
        if (this._settingsSignalId)
            this._settings.disconnect(this._settingsSignalId);
        this._settingsSignalId = 0;
        if (this._restackedSignalId)
            global.display.disconnect(this._restackedSignalId);
        this._restackedSignalId = 0;
        for (const actor of this._actors.values())
            actor.destroy();
        this._actors.clear();
    }

    _records() {
        try {
            return JSON.parse(this._settings.get_string('sticky-notes'));
        } catch (error) {
            logError(error, 'Failed to parse AnyKeep sticky note settings');
            return {};
        }
    }

    _sync() {
        const records = this._records();
        for (const [id, actor] of this._actors) {
            if (!(id in records)) {
                actor.destroy();
                this._actors.delete(id);
            }
        }
        for (const [id, geometry] of Object.entries(records)) {
            let actor = this._actors.get(id);
            if (!actor) {
                actor = new StickyNoteActor(id, geometry, this._dbus,
                    (stickyId, rect) => this._storeGeometry(stickyId, rect),
                    stickyId => this._dismiss(stickyId));
                this._actors.set(id, actor);
                global.window_group.add_child(actor.actor);
                actor.refresh();
            }
        }
        this._lowerActors();
    }

    _lowerActors() {
        const windowActors = new Set(global.get_window_actors());
        const lowestWindow = global.window_group.get_children().find(child => windowActors.has(child));
        for (const actor of this._actors.values()) {
            if (lowestWindow)
                global.window_group.set_child_below_sibling(actor.actor, lowestWindow);
            else
                global.window_group.insert_child_at_index(actor.actor, 1);
        }
    }

    _refreshAll() {
        for (const actor of this._actors.values())
            actor.refresh();
    }

    _storeGeometry(id, geometry) {
        const records = this._records();
        if (!(id in records))
            return;
        records[id] = geometry;
        this._settings.set_string('sticky-notes', JSON.stringify(records));
    }

    _dismiss(id) {
        const records = this._records();
        if (id in records) {
            delete records[id];
            this._settings.set_string('sticky-notes', JSON.stringify(records));
        }
        this._dbus.unpinStickyNote(id);
    }
}
