/*
    SPDX-License-Identifier: GPL-3.0-only
*/

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {AnyKeepWindowActivator} from './windowActivation.js';

const BUS_NAME = 'com.github.ri0n.AnyKeep';
const OBJECT_PATH = '/AnyKeep';

const ANYKEEP_IFACE_XML = `
<node>
  <interface name="com.github.ri0n.AnyKeep">
    <method name="notesJson">
      <arg type="i" name="offset" direction="in"/>
      <arg type="i" name="limit" direction="in"/>
      <arg type="s" name="query" direction="in"/>
      <arg type="s" name="response" direction="out"/>
    </method>
    <method name="globalShortcutsJson">
      <arg type="s" name="response" direction="out"/>
    </method>
    <method name="stickyNoteJson">
      <arg type="s" name="stickyId" direction="in"/>
      <arg type="s" name="response" direction="out"/>
    </method>
    <method name="openStickyNote">
      <arg type="s" name="stickyId" direction="in"/>
    </method>
    <method name="unpinStickyNote">
      <arg type="s" name="stickyId" direction="in"/>
    </method>
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
    <method name="openNote">
      <arg type="s" name="storageId" direction="in"/>
      <arg type="s" name="noteId" direction="in"/>
    </method>
    <method name="createNote"/>
    <method name="activateGlobalShortcut">
      <arg type="s" name="id" direction="in"/>
    </method>
    <method name="showNoteManager"/>
    <method name="showOptions"/>
    <method name="showAbout"/>
    <method name="quit"/>
    <signal name="notesChanged"/>
    <signal name="globalShortcutsChanged"/>
    <signal name="stickyNotesChanged"/>
  </interface>
</node>`;

const AnyKeepInterfaceInfo = Gio.DBusNodeInfo.new_for_xml(ANYKEEP_IFACE_XML).interfaces[0];

export class AnyKeepDBusClient {
    constructor() {
        this._proxy = Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SESSION,
            Gio.DBusProxyFlags.DO_NOT_AUTO_START_AT_CONSTRUCTION,
            AnyKeepInterfaceInfo,
            BUS_NAME,
            OBJECT_PATH,
            AnyKeepInterfaceInfo.name,
            null);
        this._signalIds = [];
        this._notifySignalIds = [];
        this._windowActivator = new AnyKeepWindowActivator();
    }

    destroy() {
        this._windowActivator?.destroy();
        this._windowActivator = null;

        for (const id of this._signalIds)
            this._proxy.disconnectSignal(id);
        this._signalIds = [];
        for (const id of this._notifySignalIds)
            this._proxy.disconnect(id);
        this._notifySignalIds = [];
        this._proxy = null;
    }

    onNotesChanged(callback) {
        const id = this._proxy.connectSignal('notesChanged', callback);
        this._signalIds.push(id);
    }

    onGlobalShortcutsChanged(callback) {
        const id = this._proxy.connectSignal('globalShortcutsChanged', callback);
        this._signalIds.push(id);
    }

    onStickyNotesChanged(callback) {
        const id = this._proxy.connectSignal('stickyNotesChanged', callback);
        this._signalIds.push(id);
    }

    onNameOwnerChanged(callback) {
        const id = this._proxy.connect('notify::g-name-owner', () => {
            callback(this._proxy.get_name_owner());
        });
        this._notifySignalIds.push(id);
    }

    notesJson(offset, limit, query) {
        return new Promise((resolve, reject) => {
            this._proxy.call(
                'notesJson',
                new GLib.Variant('(iis)', [offset, limit, query]),
                Gio.DBusCallFlags.NONE,
                -1,
                null,
                (proxy, result) => {
                    try {
                        resolve(proxy.call_finish(result).deepUnpack()[0]);
                    } catch (error) {
                        reject(error);
                    }
                });
        });
    }

    globalShortcutsJson() {
        return new Promise((resolve, reject) => {
            this._proxy.call(
                'globalShortcutsJson',
                null,
                Gio.DBusCallFlags.NONE,
                -1,
                null,
                (proxy, result) => {
                    try {
                        resolve(proxy.call_finish(result).deepUnpack()[0]);
                    } catch (error) {
                        reject(error);
                    }
                });
        });
    }

    stickyNoteJson(stickyId) {
        return new Promise((resolve, reject) => {
            this._proxy.call(
                'stickyNoteJson',
                new GLib.Variant('(s)', [stickyId]),
                Gio.DBusCallFlags.NONE,
                -1,
                null,
                (proxy, result) => {
                    try {
                        resolve(proxy.call_finish(result).deepUnpack()[0]);
                    } catch (error) {
                        reject(error);
                    }
                });
        });
    }

    claimWindowGeometry() {
        return new Promise((resolve, reject) => {
            this._proxy.call(
                'claimWindowGeometry',
                null,
                Gio.DBusCallFlags.NONE,
                -1,
                null,
                (proxy, result) => {
                    try {
                        resolve(proxy.call_finish(result).deepUnpack()[0]);
                    } catch (error) {
                        reject(error);
                    }
                });
        });
    }

    storeWindowGeometry(key, x, y, width, height) {
        this._call('storeWindowGeometry', new GLib.Variant('(siiii)', [key, x, y, width, height]));
    }

    windowGeometryScriptReady() {
        this._call('windowGeometryScriptReady');
    }

    processId() {
        const owner = this._proxy.get_name_owner();
        if (!owner)
            return 0;
        try {
            return Gio.DBus.session.call_sync(
                'org.freedesktop.DBus',
                '/org/freedesktop/DBus',
                'org.freedesktop.DBus',
                'GetConnectionUnixProcessID',
                new GLib.Variant('(s)', [owner]),
                null,
                Gio.DBusCallFlags.NONE,
                -1,
                null).deepUnpack()[0];
        } catch (error) {
            logError(error, 'Failed to resolve AnyKeep process ID');
            return 0;
        }
    }

    _call(method, parameters = null, activateWindow = false) {
        this._proxy.call(
            method,
            parameters,
            Gio.DBusCallFlags.NONE,
            -1,
            null,
            (proxy, result) => {
                try {
                    proxy.call_finish(result);
                    if (activateWindow)
                        this._windowActivator.requestActivation();
                } catch (error) {
                    logError(error, `Failed to call AnyKeep.${method}`);
                    return;
                }
            });
    }

    openNote(storageId, noteId) {
        this._call('openNote', new GLib.Variant('(ss)', [storageId, noteId]), true);
    }

    openStickyNote(stickyId) {
        this._call('openStickyNote', new GLib.Variant('(s)', [stickyId]), true);
    }

    unpinStickyNote(stickyId) {
        this._call('unpinStickyNote', new GLib.Variant('(s)', [stickyId]));
    }

    createNote() {
        this._call('createNote', null, true);
    }

    activateGlobalShortcut(id) {
        this._call('activateGlobalShortcut', new GLib.Variant('(s)', [id]), true);
    }

    showNoteManager() {
        this._call('showNoteManager', null, true);
    }

    showOptions() {
        this._call('showOptions', null, true);
    }

    showAbout() {
        this._call('showAbout', null, true);
    }

    quit() {
        this._call('quit');
    }
}
