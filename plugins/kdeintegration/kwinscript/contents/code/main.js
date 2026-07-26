// QtNote companion script for restoring window state on Wayland.
// Geometry and keep-above preferences are owned by QtNote; this script keeps
// only an in-memory window/key mapping.

const service = "com.github.ri0n.QtNote";
const objectPath = "/QtNote";
const interfaceName = "com.github.ri0n.QtNote";
const trackedWindows = {};

function windowId(window) {
    return String(window.internalId);
}

function isQtNoteWindow(window) {
    const desktopFile = String(window.desktopFileName || "").toLowerCase();
    const resourceClass = String(window.resourceClass || "").toLowerCase();
    return desktopFile === "qtnote" || resourceClass === "qtnote";
}

function saveGeometry(window) {
    const key = trackedWindows[windowId(window)];
    if (!key)
        return;

    const geometry = window.frameGeometry;
    callDBus(service, objectPath, interfaceName, "storeWindowGeometry",
             key,
             Math.round(geometry.x), Math.round(geometry.y),
             Math.round(geometry.width), Math.round(geometry.height));
}

function claimWindow(window) {
    const id = windowId(window);
    if (trackedWindows[id])
        return;

    callDBus(service, objectPath, interfaceName, "claimWindowGeometry", function (response) {
        if (!response || window.deleted)
            return;

        let state;
        try {
            state = JSON.parse(response);
        } catch (error) {
            print("QtNote Window Geometry: invalid response: " + error);
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
    if (!isQtNoteWindow(window) || !window.normalWindow)
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

workspace.windowAdded.connect(watchWindow);
for (const window of workspace.stackingOrder)
    watchWindow(window);

callDBus(service, objectPath, interfaceName, "windowGeometryScriptReady");
