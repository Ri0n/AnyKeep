// AnyKeep companion script for placing note windows on Wayland.
//
// AnyKeep owns the state of its note windows (geometry and keep-above) and
// tags each note window with its window id through the xdg-toplevel-tag
// protocol. The script reads all states once, then places a tagged window as
// soon as KWin adds it, before it is painted, and reports where the window is
// afterwards. Windows without a tag are not AnyKeep note windows: they are
// left to KWin.

const service = "com.github.ri0n.AnyKeep";
const objectPath = "/AnyKeep";
const interfaceName = "com.github.ri0n.AnyKeep";

let serviceOwner = "";
let states = null;        // window id -> { valid, x, y, width, height, keepAbove }
const tracked = {};       // KWin window id -> AnyKeep window id
const waiting = [];       // windows added before the states arrived

function isAnyKeepWindow(window) {
    const desktopFile = String(window.desktopFileName || "").toLowerCase();
    const resourceClass = String(window.resourceClass || "").toLowerCase();
    return desktopFile === "anykeep" || resourceClass === "anykeep";
}

function callAnyKeep(method, ...args) {
    // Use the unique connection name, not the activatable well-known name:
    // if AnyKeep has exited, a late call fails instead of starting it again.
    if (serviceOwner)
        callDBus(serviceOwner, objectPath, interfaceName, method, ...args);
}

// The stored geometry is the client area; KWin places the frame around it.
function place(window, state) {
    if (state.keepAbove !== undefined)
        window.keepAbove = Boolean(state.keepAbove);
    if (!state.valid)
        return;
    const frame = window.frameGeometry;
    const client = window.clientGeometry;
    window.frameGeometry = {
        x: state.x - (client.x - frame.x),
        y: state.y - (client.y - frame.y),
        width: state.width + (frame.width - client.width),
        height: state.height + (frame.height - client.height)
    };
}

function report(window) {
    const key = tracked[window.internalId];
    if (!key)
        return;
    const client = window.clientGeometry;
    const state = states[key] || {};
    state.valid = true;
    state.x = Math.round(client.x);
    state.y = Math.round(client.y);
    state.width = Math.round(client.width);
    state.height = Math.round(client.height);
    states[key] = state;
    callAnyKeep("storeWindowGeometry", key, state.x, state.y, state.width, state.height);
}

function adopt(window) {
    const key = String(window.tag || "");
    if (!key)
        return;
    tracked[window.internalId] = key;
    const known = states[key];
    // Place a known window right away, before it is painted.
    if (known)
        place(window, known);
    // Then ask AnyKeep, which may know better: keep-above may have changed
    // (the window is shown again to apply it), or the state was moved to a
    // new id after the script started, e.g. when a draft was published.
    callAnyKeep("windowGeometry", key, function (response) {
        if (window.deleted || tracked[window.internalId] !== key || !response)
            return;
        let state;
        try {
            state = JSON.parse(response);
        } catch (error) {
            print("AnyKeep Window Geometry: invalid window state: " + error);
            return;
        }
        if (known) {
            known.keepAbove = state.keepAbove;
            window.keepAbove = Boolean(state.keepAbove);
        } else {
            states[key] = state;
            place(window, state);
        }
    });
}

function watchWindow(window) {
    if (!isAnyKeepWindow(window) || !window.normalWindow)
        return;
    if (states === null) {
        waiting.push(window);
        return;
    }
    adopt(window);
    window.clientGeometryChanged.connect(function () { report(window); });
    window.tagChanged.connect(function () {
        // AnyKeep renamed the window (its draft was published) and moved its
        // state to the new id.
        const previous = tracked[window.internalId];
        const key = String(window.tag || "");
        if (previous && key && previous !== key && states[previous]) {
            states[key] = states[previous];
            delete states[previous];
        }
        if (key)
            tracked[window.internalId] = key;
        else
            delete tracked[window.internalId];
    });
    window.closed.connect(function () {
        report(window);
        delete tracked[window.internalId];
    });
}

function start(response) {
    try {
        states = response ? JSON.parse(response) : {};
    } catch (error) {
        print("AnyKeep Window Geometry: invalid window states: " + error);
        states = {};
    }
    workspace.windowAdded.connect(watchWindow);
    for (const window of workspace.stackingOrder) {
        if (waiting.indexOf(window) < 0)
            watchWindow(window);
    }
    while (waiting.length > 0) {
        const window = waiting.shift();
        if (!window.deleted)
            watchWindow(window);
    }
    callAnyKeep("windowGeometryScriptReady");
}

function startForCurrentOwner(owner) {
    if (states !== null || serviceOwner)
        return;
    serviceOwner = String(owner || "");
    if (!serviceOwner.startsWith(":")) {
        serviceOwner = "";
        print("AnyKeep Window Geometry: unable to resolve current D-Bus owner");
        return;
    }
    callAnyKeep("windowGeometries", start);
}

// A failed GetNameOwner call never reaches its callback, and the script has
// no timers. Ask again when the next AnyKeep window appears.
let resolvingSince = 0;
function resolveOwner() {
    resolvingSince = Date.now();
    callDBus("org.freedesktop.DBus", "/org/freedesktop/DBus",
             "org.freedesktop.DBus", "GetNameOwner", service, startForCurrentOwner);
}

// Collect AnyKeep windows appearing before the states arrive.
workspace.windowAdded.connect(function (window) {
    if (states !== null || !isAnyKeepWindow(window) || !window.normalWindow)
        return;
    waiting.push(window);
    if (!serviceOwner && Date.now() - resolvingSince > 1000)
        resolveOwner();
});

resolveOwner();
