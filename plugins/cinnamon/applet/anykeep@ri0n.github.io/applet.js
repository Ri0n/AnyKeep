const Applet = imports.ui.applet;
const Clutter = imports.gi.Clutter;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const Lang = imports.lang;
const Main = imports.ui.main;
const Mainloop = imports.mainloop;
const Pango = imports.gi.Pango;
const PopupMenu = imports.ui.popupMenu;
const St = imports.gi.St;

const BUS_NAME = 'com.github.ri0n.AnyKeep';
const OBJECT_PATH = '/AnyKeep';
const INTERFACE_NAME = 'com.github.ri0n.AnyKeep';
const PAGE_SIZE = 50;
const SEARCH_DELAY_MS = 250;
const GEOMETRY_RETRY_MS = 100;
const GEOMETRY_SAVE_DELAY_MS = 150;
const MAX_GEOMETRY_ATTEMPTS = 20;
const ACTIVATION_RETRY_MS = 100;
const MAX_ACTIVATION_ATTEMPTS = 15;

const ANYKEEP_IFACE_XML = `
<node>
  <interface name="com.github.ri0n.AnyKeep">
    <method name="notesJson">
      <arg type="i" name="offset" direction="in"/>
      <arg type="i" name="limit" direction="in"/>
      <arg type="s" name="query" direction="in"/>
      <arg type="s" name="response" direction="out"/>
    </method>
    <method name="openNote">
      <arg type="s" name="storageId" direction="in"/>
      <arg type="s" name="noteId" direction="in"/>
    </method>
    <method name="createNote"/>
    <method name="claimWindowGeometry">
      <arg type="s" name="response" direction="out"/>
    </method>
    <method name="storeWindowGeometry">
      <arg type="s" name="key" direction="in"/>
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
      <arg type="i" name="width" direction="in"/>
      <arg type="i" name="height" direction="in"/>
    </method>
    <method name="windowGeometryScriptReady"/>
    <method name="showNoteManager"/>
    <method name="showOptions"/>
    <method name="showAbout"/>
    <method name="quit"/>
    <signal name="notesChanged"/>
  </interface>
</node>`;

const AnyKeepProxy = Gio.DBusProxy.makeProxyWrapper(ANYKEEP_IFACE_XML);

function _(text) {
    return text;
}

class WindowGeometry {
    constructor(dbus) {
        this._dbus = dbus;
        this._displaySignalId = 0;
        this._mapSignalId = 0;
        this._ownerSignalId = 0;
        this._pid = 0;
        this._windows = new Map();
        this._pendingWindows = new Map();
    }

    enable() {
        this._updateOwner();
        this._ownerSignalId = this._dbus.connect('notify::g-name-owner', Lang.bind(this, function() {
            this._updateOwner();
        }));
        this._displaySignalId = global.display.connect('window-created', Lang.bind(this, function(display, window) {
            this._watch(window);
        }));
        this._mapSignalId = global.window_manager.connect('map', Lang.bind(this, function(manager, actor) {
            let window = actor.meta_window;
            let state = this._windows.get(window);
            if (state && !state.revealed) {
                state.actor = actor;
                state.mapped = false;
                actor.opacity = 0;
                if (state.geometry)
                    window.move_resize_frame(false, state.geometry.x, state.geometry.y,
                        state.geometry.width, state.geometry.height);
                Mainloop.idle_add(Lang.bind(this, function() {
                    if (this._windows.has(window)) {
                        state.mapped = true;
                        this._reveal(state);
                    }
                    return false;
                }));
            }
        }));
        let actors = global.get_window_actors();
        for (let i = 0; i < actors.length; ++i)
            this._watch(actors[i].meta_window);
        this._ready();
    }

    destroy() {
        if (this._displaySignalId)
            global.display.disconnect(this._displaySignalId);
        if (this._mapSignalId)
            global.window_manager.disconnect(this._mapSignalId);
        if (this._ownerSignalId)
            this._dbus.disconnect(this._ownerSignalId);
        for (let sourceId of this._pendingWindows.values())
            Mainloop.source_remove(sourceId);
        for (let [window, state] of this._windows)
            this._disconnect(window, state);
        this._pendingWindows.clear();
        this._windows.clear();
    }

    _updateOwner() {
        this._pid = this._processId();
        if (this._pid)
            this._ready();
    }

    _ready() {
        this._dbus.windowGeometryScriptReadyRemote(function() {});
    }

    _processId() {
        let owner = this._dbus.get_name_owner();
        if (!owner)
            return 0;
        try {
            let reply = Gio.DBus.session.call_sync(
                'org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
                new GLib.Variant('(s)', [owner]), null,
                Gio.DBusCallFlags.NONE, -1, null);
            return reply.deep_unpack()[0];
        } catch (error) {
            global.logError(error, 'Failed to resolve AnyKeep process ID');
            return 0;
        }
    }

    _isAnyKeep(window) {
        if (!this._pid)
            this._pid = this._processId();
        let wmClass = window.get_wm_class();
        return (this._pid && window.get_pid() === this._pid)
            || (wmClass && wmClass.toLowerCase() === 'anykeep');
    }

    _watch(window) {
        if (!window || this._windows.has(window) || this._pendingWindows.has(window))
            return;
        let attempt = 0;
        let tryWatch = Lang.bind(this, function() {
            attempt++;
            if (this._isAnyKeep(window)) {
                this._pendingWindows.delete(window);
                this._startWatching(window);
                return false;
            }
            if (attempt >= MAX_GEOMETRY_ATTEMPTS) {
                this._pendingWindows.delete(window);
                return false;
            }
            return true;
        });
        if (!tryWatch())
            return;
        this._pendingWindows.set(window, Mainloop.timeout_add(GEOMETRY_RETRY_MS, tryWatch));
    }

    _startWatching(window) {
        let actor = window.get_compositor_private();
        let state = {key: '', claimAttempt: 0, claimSourceId: 0, saveSourceId: 0,
            signalIds: [], actor: actor, mapped: actor ? actor.mapped : false,
            revealed: false, geometry: null};
        if (actor)
            actor.opacity = 0;
        state.signalIds.push(window.connect('position-changed', Lang.bind(this, function() { this._changed(window); })));
        state.signalIds.push(window.connect('size-changed', Lang.bind(this, function() { this._changed(window); })));
        // Save while Meta.Window still exposes its final frame. By 'unmanaged'
        // Muffin may already have torn the frame down to an empty rectangle.
        state.signalIds.push(window.connect('unmanaging', Lang.bind(this, function() { this._unmanaged(window); })));
        this._windows.set(window, state);
        if (!this._claimSync(window, state))
            this._claim(window);
    }

    _claimSync(window, state) {
        try {
            let reply = Gio.DBus.session.call_sync(
                BUS_NAME, OBJECT_PATH, INTERFACE_NAME, 'claimWindowGeometry',
                null, null, Gio.DBusCallFlags.NONE, 250, null);
            let unpacked = reply.deep_unpack();
            let payload = unpacked.length > 0 ? unpacked[0] : '';
            if (!payload)
                return false;
            let geometry = JSON.parse(payload);
            if (!geometry.key)
                return false;
            state.key = geometry.key;
            if (geometry.valid) {
                state.geometry = geometry;
                window.move_resize_frame(false, geometry.x, geometry.y, geometry.width, geometry.height);
            } else {
                state.geometry = this._initialGeometry(window);
                window.move_resize_frame(false, state.geometry.x, state.geometry.y,
                    state.geometry.width, state.geometry.height);
            }
            this._reveal(state);
            return true;
        } catch (error) {
            global.logError(error, 'Failed to synchronously claim AnyKeep window geometry');
            return false;
        }
    }

    _claim(window) {
        let state = this._windows.get(window);
        if (!state || state.key || state.claiming)
            return;
        state.claiming = true;
        state.claimAttempt++;
        this._dbus.claimWindowGeometryRemote(Lang.bind(this, function(response, error) {
            state.claiming = false;
            if (!this._windows.has(window))
                return;
            let payload = Array.isArray(response) ? response[0] : response;
            if (error || !payload) {
                this._reveal(state);
                this._retryClaim(window, state);
                return;
            }
            try {
                let geometry = JSON.parse(payload);
                if (!geometry.key) {
                    this._reveal(state);
                    this._retryClaim(window, state);
                    return;
                }
                state.key = geometry.key;
                if (geometry.valid) {
                    state.geometry = geometry;
                    window.move_resize_frame(false, geometry.x, geometry.y, geometry.width, geometry.height);
                } else {
                    state.geometry = this._initialGeometry(window);
                    window.move_resize_frame(false, state.geometry.x, state.geometry.y,
                        state.geometry.width, state.geometry.height);
                }
                this._reveal(state);
            } catch (parseError) {
                this._reveal(state);
                global.logError(parseError, 'Failed to parse AnyKeep window geometry');
            }
        }));
    }

    _reveal(state) {
        if (state.revealed || !state.actor || !state.mapped)
            return;
        state.revealed = true;
        state.actor.opacity = 255;
    }

    _initialGeometry(window) {
        let rect = window.get_frame_rect();
        let workarea = window.get_work_area_current_monitor();
        let width = Math.min(workarea.width, Math.max(320, rect.width));
        let height = Math.min(workarea.height, Math.max(240, rect.height));
        let xMin = workarea.x + Math.floor((workarea.width - width) / 4);
        let xMax = workarea.x + Math.floor((workarea.width - width) / 2);
        let yMin = workarea.y + Math.floor((workarea.height - height) / 4);
        let yMax = workarea.y + Math.floor((workarea.height - height) / 2);
        return {
            x: GLib.random_int_range(xMin, Math.max(xMin + 1, xMax + 1)),
            y: GLib.random_int_range(yMin, Math.max(yMin + 1, yMax + 1)),
            width: width,
            height: height
        };
    }

    _retryClaim(window, state) {
        if (state.claimSourceId || state.claimAttempt >= MAX_GEOMETRY_ATTEMPTS)
            return;
        state.claimSourceId = Mainloop.timeout_add(GEOMETRY_RETRY_MS, Lang.bind(this, function() {
            state.claimSourceId = 0;
            this._claim(window);
            return false;
        }));
    }

    _changed(window) {
        let state = this._windows.get(window);
        if (!state)
            return;
        if (!state.key) {
            this._claim(window);
            return;
        }
        if (state.saveSourceId)
            Mainloop.source_remove(state.saveSourceId);
        state.saveSourceId = Mainloop.timeout_add(GEOMETRY_SAVE_DELAY_MS, Lang.bind(this, function() {
            state.saveSourceId = 0;
            this._save(window, state);
            return false;
        }));
    }

    _save(window, state) {
        if (!state.key)
            return;
        let rect = window.get_frame_rect();
        this._dbus.storeWindowGeometryRemote(state.key, rect.x, rect.y, rect.width, rect.height, function() {});
    }

    _unmanaged(window) {
        let state = this._windows.get(window);
        if (!state)
            return;
        let rect = window.get_frame_rect();
        if (state.key) {
            this._dbus.storeWindowGeometryRemote(
                state.key, rect.x, rect.y, rect.width, rect.height, function() {});
        } else {
            this._dbus.claimWindowGeometryRemote(Lang.bind(this, function(response, error) {
                let payload = Array.isArray(response) ? response[0] : response;
                if (error || !payload)
                    return;
                try {
                    let geometry = JSON.parse(payload);
                    if (geometry.key) {
                        this._dbus.storeWindowGeometryRemote(
                            geometry.key, rect.x, rect.y, rect.width, rect.height, function() {});
                    }
                } catch (parseError) {
                    global.logError(parseError, 'Failed to save closing AnyKeep window geometry');
                }
            }));
        }
        this._reveal(state);
        this._disconnect(window, state);
        this._windows.delete(window);
    }

    _disconnect(window, state) {
        if (state.claimSourceId)
            Mainloop.source_remove(state.claimSourceId);
        if (state.saveSourceId)
            Mainloop.source_remove(state.saveSourceId);
        for (let i = 0; i < state.signalIds.length; ++i) {
            try { window.disconnect(state.signalIds[i]); } catch (error) {}
        }
    }
}

class AnyKeepApplet extends Applet.IconApplet {
    constructor(metadata, orientation, panelHeight, instanceId) {
        super(orientation, panelHeight, instanceId);

        this.set_applet_icon_symbolic_name('anykeep-symbolic');
        this.set_applet_tooltip('AnyKeep');

        this._query = '';
        this._items = [];
        this._noteRows = [];
        this._available = true;
        this._loading = false;
        this._loadingMore = false;
        this._hasMore = false;
        this._requestSerial = 0;
        this._searchSourceId = 0;
        this._focusSourceId = 0;
        this._signalIds = [];
        this._windowGeometry = null;
        this._activationSourceId = 0;

        this._dbus = new AnyKeepProxy(
            Gio.DBus.session,
            BUS_NAME,
            OBJECT_PATH,
            Lang.bind(this, this._onProxyReady));

        this.menuManager = new PopupMenu.PopupMenuManager(this);
        this.menu = new Applet.AppletPopupMenu(this, orientation);
        this.menuManager.addMenu(this.menu);

        this._buildMenu();
        this._buildContextMenu();
        this.menu.connect('open-state-changed', Lang.bind(this, function(menu, open) {
            if (!open)
                return;

            this._refresh();
            this._scheduleSearchFocus();
        }));
    }

    on_applet_clicked() {
        this.menu.toggle();
    }

    on_applet_middle_clicked() {
        this._call('createNote');
    }

    on_applet_removed_from_panel() {
        this._destroy();
    }

    _destroy() {
        if (this._searchSourceId) {
            Mainloop.source_remove(this._searchSourceId);
            this._searchSourceId = 0;
        }
        if (this._focusSourceId) {
            Mainloop.source_remove(this._focusSourceId);
            this._focusSourceId = 0;
        }
        for (let i = 0; i < this._signalIds.length; ++i)
            this._dbus.disconnectSignal(this._signalIds[i]);
        this._signalIds = [];
        if (this._windowGeometry) {
            this._windowGeometry.destroy();
            this._windowGeometry = null;
        }
        if (this._activationSourceId) {
            Mainloop.source_remove(this._activationSourceId);
            this._activationSourceId = 0;
        }
    }

    _onProxyReady(proxy, error) {
        if (error) {
            global.logError(error, 'Failed to create AnyKeep DBus proxy');
            this._available = false;
            this._render();
            return;
        }

        this._signalIds.push(this._dbus.connectSignal('notesChanged', Lang.bind(this, this._refresh)));
        this._windowGeometry = new WindowGeometry(this._dbus);
        this._windowGeometry.enable();
        this._refresh();
    }

    _buildMenu() {
        this.menu.removeAll();

        this._rootSection = new PopupMenu.PopupMenuSection();
        this.menu.addMenuItem(this._rootSection);

        this._rootBox = new St.BoxLayout({
            vertical: true,
            style_class: 'anykeep-popup',
        });
        this._rootSection.actor.add_actor(this._rootBox);

        this._header = new St.BoxLayout({
            vertical: true,
            style_class: 'anykeep-header',
        });
        this._rootBox.add_actor(this._header);

        this._actions = new St.BoxLayout({
            style_class: 'anykeep-actions',
        });
        this._header.add_actor(this._actions);
        this._addAction(_('New Note'), 'list-add-symbolic', Lang.bind(this, function() { this._call('createNote'); }));
        this._addAction(_('Note Manager'), 'view-list-symbolic', Lang.bind(this, function() { this._call('showNoteManager'); }));
        this._addAction(_('Configure AnyKeep...'), 'preferences-system-symbolic', Lang.bind(this, function() { this._call('showOptions'); }));
        this._addAction(_('About AnyKeep'), 'help-about-symbolic', Lang.bind(this, function() { this._call('showAbout'); }));

        this._searchEntry = new St.Entry({
            name: 'menu-search-entry',
            hint_text: _('Search notes'),
            can_focus: true,
            track_hover: true,
            style_class: 'anykeep-search-entry',
        });
        this._header.add_actor(this._searchEntry);
        this._searchEntry.clutter_text.connect('text-changed', Lang.bind(this, function() {
            this._setQuery(this._searchEntry.get_text());
        }));
        this._searchEntry.clutter_text.connect('key-press-event', Lang.bind(this, this._onSearchKeyPress));

        this._headerSeparator = new St.Widget({
            style_class: 'anykeep-header-separator',
        });
        this._rootBox.add_actor(this._headerSeparator);

        this._scrollView = new St.ScrollView({
            hscrollbar_policy: St.PolicyType.NEVER,
            vscrollbar_policy: St.PolicyType.AUTOMATIC,
            overlay_scrollbars: true,
            style_class: 'anykeep-scroll-view',
        });
        this._scrollView.set_clip_to_allocation(true);
        this._list = new St.BoxLayout({
            vertical: true,
            style_class: 'anykeep-notes-list',
        });
        this._scrollView.add_actor(this._list);
        this._rootBox.add_actor(this._scrollView);
    }

    _addAction(label, iconName, callback) {
        let button = new St.Button({
            accessible_name: label,
            can_focus: true,
            style_class: 'notification-icon-button anykeep-action-button',
        });
        let icon = new St.Icon({
            icon_name: iconName,
            icon_size: 16,
            icon_type: St.IconType.SYMBOLIC,
            style_class: 'anykeep-action-icon',
        });
        button.set_child(icon);
        button.connect('clicked', Lang.bind(this, function() {
            callback();
            this.menu.close();
        }));
        this._actions.add_actor(button);
    }

    _buildContextMenu() {
        this._addContextAction(_('New Note'), 'list-add-symbolic', 'createNote');
        this._addContextAction(_('Note Manager'), 'view-list-symbolic', 'showNoteManager');
        this._addContextAction(_('Configure AnyKeep...'), 'preferences-system-symbolic', 'showOptions');
        this._addContextAction(_('About AnyKeep'), 'help-about-symbolic', 'showAbout');
    }

    _addContextAction(label, iconName, method) {
        let item = new PopupMenu.PopupIconMenuItem(label, iconName, St.IconType.SYMBOLIC);
        item.connect('activate', Lang.bind(this, function() {
            this._call(method);
        }));
        this._applet_context_menu.addMenuItem(item);
    }

    _scheduleSearchFocus() {
        if (this._focusSourceId)
            Mainloop.source_remove(this._focusSourceId);

        this._focusSourceId = Mainloop.timeout_add(50, Lang.bind(this, function() {
            this._focusSourceId = 0;
            if (this.menu.isOpen) {
                this._searchEntry.grab_key_focus();
                this._searchEntry.clutter_text.grab_key_focus();
            }
            return false;
        }));
    }

    _setQuery(query) {
        query = query.trim();
        if (this._query === query)
            return;

        this._query = query;
        this._loading = true;
        this._loadingMore = false;
        this._requestSerial++;
        this._render();

        if (this._searchSourceId)
            Mainloop.source_remove(this._searchSourceId);
        this._searchSourceId = Mainloop.timeout_add(SEARCH_DELAY_MS, Lang.bind(this, function() {
            this._searchSourceId = 0;
            this._refresh();
            return false;
        }));
    }

    _refresh() {
        if (this._searchSourceId) {
            Mainloop.source_remove(this._searchSourceId);
            this._searchSourceId = 0;
        }
        this._requestPage(0, false);
    }

    _loadMore() {
        if (!this._hasMore || this._loading || this._loadingMore)
            return;

        this._requestPage(this._items.length, true);
    }

    _requestPage(offset, append) {
        if (!this._dbus)
            return;

        if (append)
            this._loadingMore = true;
        else
            this._loading = true;
        this._render();

        let requestSerial = ++this._requestSerial;
        this._dbus.notesJsonRemote(offset, PAGE_SIZE, this._query, Lang.bind(this, function(response, error) {
            if (requestSerial !== this._requestSerial)
                return;

            this._loading = false;
            this._loadingMore = false;
            if (error) {
                let errorMessage = String(error);
                if (errorMessage.indexOf('org.freedesktop.DBus.Error.UnknownObject') < 0
                    && errorMessage.indexOf('org.freedesktop.DBus.Error.ServiceUnknown') < 0) {
                    global.logError(error, 'Failed to fetch AnyKeep notes');
                }
                this._available = false;
                this._hasMore = false;
                if (!append)
                    this._items = [];
                this._render();
                return;
            }

            try {
                let parsed = JSON.parse(response);
                let notes = Array.isArray(parsed.notes) ? parsed.notes : [];
                this._available = true;
                this._hasMore = parsed.hasMore === true;
                this._items = append ? this._items.concat(notes) : notes;
            } catch (parseError) {
                global.logError(parseError, 'Failed to parse AnyKeep notes');
                this._available = false;
                this._hasMore = false;
                if (!append)
                    this._items = [];
            }
            this._render();
        }));
    }

    _render() {
        if (!this._list)
            return;

        this._list.destroy_all_children();
        this._noteRows = [];

        if (!this._available) {
            this._list.add_actor(this._message(_('AnyKeep is not running')));
            return;
        }

        if (!this._loading && this._items.length === 0) {
            this._list.add_actor(this._message(this._query ? _('No notes match the search') : _('No notes yet')));
            return;
        }

        for (let i = 0; i < this._items.length; ++i) {
            let row = this._noteRow(this._items[i], i);
            this._noteRows.push(row);
            this._list.add_actor(row);
        }

        if (this._loading || this._loadingMore)
            this._list.add_actor(this._message(_('Loading...')));

        if (this._hasMore && !this._loading && !this._loadingMore)
            this._list.add_actor(this._loadMoreRow());
    }

    _noteRow(note, index) {
        let row = new St.Button({
            can_focus: true,
            x_expand: true,
            x_align: Clutter.ActorAlign.FILL,
            style_class: 'anykeep-note-row',
        });
        let title = note.title && note.title.length > 0 ? note.title : _('Untitled Note');
        let titleLabel = new St.Label({
            text: title,
            x_expand: true,
            x_align: Clutter.ActorAlign.FILL,
            y_align: Clutter.ActorAlign.CENTER,
        });
        titleLabel.clutter_text.set_line_alignment(
            St.Widget.get_default_direction() === St.TextDirection.RTL
                ? Pango.Alignment.RIGHT
                : Pango.Alignment.LEFT);
        row.set_child(titleLabel);
        row.connect('clicked', Lang.bind(this, function() {
            this._activateNote(index);
        }));
        row.connect('key-press-event', Lang.bind(this, function(actor, event) {
            return this._onNoteKeyPress(index, event);
        }));
        return row;
    }

    _onSearchKeyPress(actor, event) {
        let symbol = event.get_key_symbol();
        if (symbol === Clutter.KEY_Down || symbol === Clutter.KEY_KP_Down) {
            if (this._noteRows.length > 0)
                this._focusNote(this._visibleNoteIndex(false));
            return true;
        }
        if (symbol === Clutter.KEY_Up || symbol === Clutter.KEY_KP_Up) {
            if (this._noteRows.length > 0)
                this._focusNote(this._visibleNoteIndex(true));
            return true;
        }
        if (symbol === Clutter.KEY_Return || symbol === Clutter.KEY_KP_Enter) {
            if (this._items.length > 0)
                this._activateNote(0);
            return true;
        }
        return false;
    }

    _onNoteKeyPress(index, event) {
        let symbol = event.get_key_symbol();
        if (symbol === Clutter.KEY_Down || symbol === Clutter.KEY_KP_Down) {
            if (index + 1 < this._noteRows.length)
                this._focusNote(index + 1);
            return true;
        }
        if (symbol === Clutter.KEY_Up || symbol === Clutter.KEY_KP_Up) {
            if (index > 0) {
                this._focusNote(index - 1);
            } else {
                this._searchEntry.clutter_text.grab_key_focus();
            }
            return true;
        }
        if (symbol === Clutter.KEY_Return || symbol === Clutter.KEY_KP_Enter) {
            this._activateNote(index);
            return true;
        }
        return false;
    }

    _visibleNoteIndex(fromEnd) {
        let adjustment = this._scrollView.get_vscroll_bar().get_adjustment();
        let top = adjustment.get_value();
        let viewportBox = this._scrollView.get_allocation_box();
        let bottom = top + viewportBox.y2 - viewportBox.y1;

        if (fromEnd) {
            for (let i = this._noteRows.length - 1; i >= 0; --i) {
                if (this._noteRows[i].get_allocation_box().y1 < bottom)
                    return i;
            }
            return this._noteRows.length - 1;
        }

        for (let i = 0; i < this._noteRows.length; ++i) {
            if (this._noteRows[i].get_allocation_box().y2 > top)
                return i;
        }
        return 0;
    }

    _focusNote(index) {
        let row = this._noteRows[index];
        if (!row)
            return;

        row.grab_key_focus();
        let adjustment = this._scrollView.get_vscroll_bar().get_adjustment();
        let viewportBox = this._scrollView.get_allocation_box();
        let viewportHeight = viewportBox.y2 - viewportBox.y1;
        let rowBox = row.get_allocation_box();
        let currentValue = adjustment.get_value();
        let newValue = currentValue;

        if (rowBox.y1 < currentValue)
            newValue = rowBox.y1;
        else if (rowBox.y2 > currentValue + viewportHeight)
            newValue = rowBox.y2 - viewportHeight;

        if (newValue !== currentValue)
            adjustment.set_value(newValue);
    }

    _activateNote(index) {
        let note = this._items[index];
        if (!note)
            return;

        this._call('openNote', note.storageId, note.id);
        this.menu.close();
    }

    _message(text) {
        return new St.Label({
            text: text,
            x_align: Clutter.ActorAlign.CENTER,
            style_class: 'anykeep-message',
        });
    }

    _loadMoreRow() {
        let row = new St.Button({
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
        row.connect('clicked', Lang.bind(this, this._loadMore));
        return row;
    }

    _call(method) {
        if (!this._dbus)
            return;

        let args = Array.prototype.slice.call(arguments, 1);
        let callback = Lang.bind(this, function(result, error) {
            if (error)
                global.logError(error, 'Failed to call AnyKeep.' + method);
            else if (method === 'openNote' || method === 'createNote')
                this._requestActivation();
        });

        switch (method) {
        case 'openNote':
            this._dbus.openNoteRemote(args[0], args[1], callback);
            break;
        case 'createNote':
            this._dbus.createNoteRemote(callback);
            break;
        case 'showNoteManager':
            this._dbus.showNoteManagerRemote(callback);
            break;
        case 'showOptions':
            this._dbus.showOptionsRemote(callback);
            break;
        case 'showAbout':
            this._dbus.showAboutRemote(callback);
            break;
        case 'quit':
            this._dbus.quitRemote(callback);
            break;
        default:
            break;
        }
    }

    _requestActivation() {
        if (this._activationSourceId)
            Mainloop.source_remove(this._activationSourceId);
        let attempt = 0;
        this._activationSourceId = Mainloop.timeout_add(ACTIVATION_RETRY_MS, Lang.bind(this, function() {
            attempt++;
            let windows = [];
            let actors = global.get_window_actors();
            let pid = this._windowGeometry ? this._windowGeometry._pid : 0;
            for (let i = 0; i < actors.length; ++i) {
                let window = actors[i].meta_window;
                let wmClass = window.get_wm_class();
                if ((pid && window.get_pid() === pid) || (wmClass && wmClass.toLowerCase() === 'anykeep'))
                    windows.push(window);
            }
            let window = windows.find(function(item) { return item.demands_attention || item.urgent; });
            if (!window && attempt >= 3 && windows.length > 0) {
                windows.sort(function(a, b) { return b.get_user_time() - a.get_user_time(); });
                window = windows[0];
            }
            if (window) {
                Main.activateWindow(window, global.get_current_time());
                this._activationSourceId = 0;
                return false;
            }
            if (attempt >= MAX_ACTIVATION_ATTEMPTS) {
                this._activationSourceId = 0;
                return false;
            }
            return true;
        }));
    }
}

function main(metadata, orientation, panelHeight, instanceId) {
    return new AnyKeepApplet(metadata, orientation, panelHeight, instanceId);
}
