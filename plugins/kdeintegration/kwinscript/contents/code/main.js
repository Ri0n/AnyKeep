// AnyKeep companion script for restoring window state on Wayland.
// Geometry and keep-above preferences are owned by AnyKeep; this script keeps
// only an in-memory window/key mapping.

const service = "com.github.ri0n.AnyKeep";
const objectPath = "/AnyKeep";
const interfaceName = "com.github.ri0n.AnyKeep";
const trackedWindows = {};
let serviceOwner = "";

function windowId(window) {
    return String(window.internalId);
}

function isAnyKeepWindow(window) {
    const desktopFile = String(window.desktopFileName || "").toLowerCase();
    const resourceClass = String(window.resourceClass || "").toLowerCase();
    return desktopFile === "anykeep" || resourceClass === "anykeep";
}

function saveGeometry(window) {
    const key = trackedWindows[windowId(window)];
    if (!key)
        return;

    const geometry = window.frameGeometry;
    if (!serviceOwner)
        return;

    callDBus(serviceOwner, objectPath, interfaceName, "storeWindowGeometry",
             key,
             Math.round(geometry.x), Math.round(geometry.y),
             Math.round(geometry.width), Math.round(geometry.height));
}

function claimWindow(window) {
    const id = windowId(window);
    if (trackedWindows[id])
        return;

    if (!serviceOwner)
        return;

    callDBus(serviceOwner, objectPath, interfaceName, "claimWindowGeometry", function (response) {
        if (!response || window.deleted)
            return;

        let state;
        try {
            state = JSON.parse(response);
        } catch (error) {
            print("AnyKeep Window Geometry: invalid response: " + error);
            return;
        }
        if (!state.key)
            return;

        trackedWindows[id] = state.key;
        if (state.keepAbove !== undefined)
            window.keepAbove = Boolean(state.keepAbove);
        if (state.valid) {
            window.frameGeometry = {
                x: state.x,
                y: state.y,
                width: state.width,
                height: state.height
            };
        }
    });
}

function watchWindow(window) {
    if (!isAnyKeepWindow(window) || !window.normalWindow)
        return;

    claimWindow(window);
    window.frameGeometryChanged.connect(function () {
        if (trackedWindows[windowId(window)])
            saveGeometry(window);
        else
            claimWindow(window);
    });
    window.closed.connect(function () {
        saveGeometry(window);
        delete trackedWindows[windowId(window)];
    });
}

function startForCurrentOwner(owner) {
    serviceOwner = String(owner || "");
    if (!serviceOwner.startsWith(":")) {
        print("AnyKeep Window Geometry: unable to resolve current D-Bus owner");
        return;
    }

    workspace.windowAdded.connect(watchWindow);
    for (const window of workspace.stackingOrder)
        watchWindow(window);

    // Use the unique connection name, not the activatable well-known name.
    // If AnyKeep exits while a window.closed/frameGeometryChanged callback is
    // still queued, the call then fails instead of asking D-Bus to start a new
    // AnyKeep process.
    callDBus(serviceOwner, objectPath, interfaceName, "windowGeometryScriptReady");
}

callDBus("org.freedesktop.DBus", "/org/freedesktop/DBus",
         "org.freedesktop.DBus", "GetNameOwner", service, startForCurrentOwner);
