const Desklet = imports.ui.desklet;
const Clutter = imports.gi.Clutter;
const Gio = imports.gi.Gio;
const Lang = imports.lang;
const Main = imports.ui.main;
const Pango = imports.gi.Pango;
const PopupMenu = imports.ui.popupMenu;
const Settings = imports.ui.settings;
const St = imports.gi.St;

const BUS_NAME = 'com.github.ri0n.AnyKeep';
const OBJECT_PATH = '/AnyKeep';
const GRIP_TRANSLATION_X = 0;
const GRIP_TRANSLATION_Y = 0;

const ANYKEEP_IFACE_XML = `
<node>
  <interface name="com.github.ri0n.AnyKeep">
    <method name="stickyNoteForPresentationJson">
      <arg type="s" name="presentationId" direction="in"/>
      <arg type="s" name="response" direction="out"/>
    </method>
    <method name="openStickyNote">
      <arg type="s" name="stickyId" direction="in"/>
    </method>
    <method name="unpinStickyNote">
      <arg type="s" name="stickyId" direction="in"/>
    </method>
    <signal name="stickyNotesChanged"/>
  </interface>
</node>`;

const AnyKeepProxy = Gio.DBusProxy.makeProxyWrapper(ANYKEEP_IFACE_XML);

function _(text) {
    return text;
}

class AnyKeepStickyDesklet extends Desklet.Desklet {
    constructor(metadata, deskletId) {
        super(metadata, deskletId);
        this._stickyId = '';
        this._signalId = 0;
        this._ownerSignalId = 0;
        this.width = 300;
        this.height = 180;

        this._settings = new Settings.DeskletSettings(this, metadata.uuid, this.instance_id);
        this._settings.bind('width', 'width', Lang.bind(this, this._applySize));
        this._settings.bind('height', 'height', Lang.bind(this, this._applySize));

        this._resizeEventActor = new Clutter.Actor({width: 0, height: 0, reactive: true});
        Main.uiGroup.add_actor(this._resizeEventActor);
        this._resizeEventId = this._resizeEventActor.connect('event', Lang.bind(this, this._handleResizeEvent));

        this._box = new St.Widget({
            reactive: true,
            layout_manager: new Clutter.BinLayout({
                x_align: Clutter.BinAlignment.FILL,
                y_align: Clutter.BinAlignment.FILL,
            }),
            style_class: 'anykeep-sticky',
        });
        this._content = new St.BoxLayout({
            vertical: true,
            x_expand: true,
            y_expand: true,
            style_class: 'anykeep-sticky-content',
        });
        this._title = new St.Label({style_class: 'anykeep-sticky-title'});
        this._title.clutter_text.ellipsize = Pango.EllipsizeMode.END;
        this._content.add_actor(this._title);

        this._body = new St.Label({
            style_class: 'anykeep-sticky-body',
            y_align: St.Align.START,
            y_expand: true,
        });
        this._body.clutter_text.line_wrap = true;
        this._body.clutter_text.line_wrap_mode = Pango.WrapMode.WORD_CHAR;
        this._body.clutter_text.ellipsize = Pango.EllipsizeMode.END;
        this._content.add_actor(this._body);
        this._box.add_actor(this._content);

        this._resizeGrip = new St.DrawingArea({
            reactive: true,
            x_align: St.Align.END,
            y_align: St.Align.END,
            translation_x: GRIP_TRANSLATION_X,
            translation_y: GRIP_TRANSLATION_Y,
            style_class: 'anykeep-sticky-resize-grip',
        });
        this._resizeGrip.set_size(18, 18);
        this._resizeGrip.connect('repaint', Lang.bind(this, this._paintResizeGrip));
        this._resizeGrip.connect('button-press-event', Lang.bind(this, this._startResize));
        this._gripLayer = new St.Bin({
            reactive: false,
            x_align: St.Align.END,
            y_align: St.Align.END,
            style_class: 'anykeep-sticky-grip-layer',
        });
        this._gripLayer.set_child(this._resizeGrip);
        this._box.add_actor(this._gripLayer);
        this.setContent(this._box);
        this._applySize();

        let editItem = new PopupMenu.PopupIconMenuItem(_('Edit Note'), 'document-edit-symbolic', St.IconType.SYMBOLIC);
        editItem.connect('activate', Lang.bind(this, this._open));
        this._menu.addMenuItem(editItem);
        let unpinItem = new PopupMenu.PopupIconMenuItem(_('Unpin'), 'window-close-symbolic', St.IconType.SYMBOLIC);
        unpinItem.connect('activate', Lang.bind(this, this._unpin));
        this._menu.addMenuItem(unpinItem);

        this._proxy = new AnyKeepProxy(Gio.DBus.session, BUS_NAME, OBJECT_PATH,
            Lang.bind(this, this._onProxyReady));
        this._showMessage(_('Connecting to AnyKeep…'));
    }

    _onProxyReady(proxy, error) {
        if (error) {
            global.logError(error, 'Failed to connect AnyKeep sticky desklet');
            this._showMessage(_('AnyKeep is not running'));
            return;
        }
        this._signalId = this._proxy.connectSignal('stickyNotesChanged', Lang.bind(this, this._refresh));
        this._ownerSignalId = this._proxy.connect('notify::g-name-owner', Lang.bind(this, function() {
            if (this._proxy.get_name_owner())
                this._refresh();
            else
                this._showMessage(_('AnyKeep is not running'));
        }));
        this._refresh();
    }

    _refresh() {
        this._proxy.stickyNoteForPresentationJsonRemote(String(this.instance_id),
            Lang.bind(this, function(result, error) {
                if (error) {
                    global.logError(error, 'Failed to load AnyKeep sticky note');
                    this._showMessage(_('Unable to load note'));
                    return;
                }
                try {
                    let payload = Array.isArray(result) ? result[0] : result;
                    let note = JSON.parse(payload || '{}');
                    if (!note.stickyId) {
                        this._showMessage(_('Note is unavailable'));
                        return;
                    }
                    this._stickyId = note.stickyId;
                    this._title.text = note.title || _('[No Title]');
                    this._body.text = note.body || '';
                } catch (parseError) {
                    global.logError(parseError, 'Failed to parse AnyKeep sticky note');
                    this._showMessage(_('Unable to load note'));
                }
            }));
    }

    _showMessage(message) {
        this._title.text = _('AnyKeep');
        this._body.text = message;
    }

    _applySize() {
        if (this._box)
            this._box.set_size(this.width, this.height);
    }

    _paintResizeGrip(area) {
        const cr = area.get_context();
        const [width, height] = area.get_surface_size();
        const color = area.get_theme_node().get_foreground_color();
        Clutter.cairo_set_source_color(cr, color);
        cr.setLineWidth(1.5);
        for (const radius of [5, 9, 13]) {
            cr.arc(width, height, radius, Math.PI, Math.PI * 1.5);
            cr.stroke();
        }
        cr.$dispose();
    }

    _startResize(actor, event) {
        if (event.get_button() !== Clutter.BUTTON_PRIMARY)
            return Clutter.EVENT_PROPAGATE;

        let [x, y] = event.get_coords();
        this._resizeStart = {x: x, y: y, width: this.width, height: this.height};
        this._draggable.inhibit = true;
        this._resizeEventsGrabbed = Main.pushModal(this._resizeEventActor);
        this._resizeDevice = event.get_device();
        if (this._resizeEventsGrabbed)
            this._resizeDevice.grab(this._resizeEventActor);
        return Clutter.EVENT_STOP;
    }

    _handleResizeEvent(actor, event) {
        if (!this._resizeStart)
            return Clutter.EVENT_PROPAGATE;
        if (event.type() === Clutter.EventType.MOTION) {
            let [x, y] = event.get_coords();
            let width = Math.max(220, Math.round(this._resizeStart.width + x - this._resizeStart.x));
            let height = Math.max(140, Math.round(this._resizeStart.height + y - this._resizeStart.y));
            this._box.set_size(width, height);
            return Clutter.EVENT_STOP;
        }
        if (event.type() === Clutter.EventType.BUTTON_RELEASE) {
            let [x, y] = event.get_coords();
            this.width = Math.max(220, Math.round(this._resizeStart.width + x - this._resizeStart.x));
            this.height = Math.max(140, Math.round(this._resizeStart.height + y - this._resizeStart.y));
            this._stopResize();
            this._applySize();
            return Clutter.EVENT_STOP;
        }
        if (event.type() === Clutter.EventType.KEY_PRESS && event.get_key_symbol() === Clutter.KEY_Escape) {
            this._stopResize();
            this._applySize();
            return Clutter.EVENT_STOP;
        }
        return Clutter.EVENT_PROPAGATE;
    }

    _stopResize() {
        if (this._resizeDevice) {
            if (this._resizeEventsGrabbed)
                this._resizeDevice.ungrab();
            this._resizeDevice = null;
        }
        if (this._resizeEventsGrabbed) {
            Main.popModal(this._resizeEventActor);
            this._resizeEventsGrabbed = false;
        }
        if (this._draggable)
            this._draggable.inhibit = false;
        this._resizeStart = null;
    }

    _open() {
        if (this._stickyId)
            this._proxy.openStickyNoteRemote(this._stickyId, function() {});
    }

    _unpin() {
        if (this._stickyId)
            this._proxy.unpinStickyNoteRemote(this._stickyId, function() {});
    }

    on_desklet_clicked(event) {
        if (event.get_click_count && event.get_click_count() >= 2)
            this._open();
    }

    on_desklet_removed(deleteConfig) {
        this._stopResize();
        if (this._resizeEventId) {
            this._resizeEventActor.disconnect(this._resizeEventId);
            this._resizeEventId = 0;
        }
        if (this._resizeEventActor) {
            this._resizeEventActor.destroy();
            this._resizeEventActor = null;
        }
        if (this._signalId)
            this._proxy.disconnectSignal(this._signalId);
        if (this._ownerSignalId)
            this._proxy.disconnect(this._ownerSignalId);
        if (deleteConfig && this._stickyId)
            this._proxy.unpinStickyNoteRemote(this._stickyId, function() {});
        this._signalId = 0;
        this._ownerSignalId = 0;
    }
}

function main(metadata, deskletId) {
    return new AnyKeepStickyDesklet(metadata, deskletId);
}
