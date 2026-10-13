import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The images of a post: its signed thumbnails, shown at once (they came with
// the post), and the full image only when the reader asks for it. Fetching a
// full image means connecting to whoever stores it on Logos Storage, so it
// waits for a click, with a line saying so. Every picture here is a data: URL
// the backend wrote itself after decoding and re-encoding; nothing is ever
// loaded from a URL in a post.
ColumnLayout {
    id: strip
    property var app            // Main.qml's root
    property var post           // a post from thread(): images, imagesMissing, id
    readonly property var images: post && post.images ? post.images : []
    readonly property int missing: post && post.imagesMissing ? post.imagesMissing : 0
    visible: images.length > 0 || missing > 0
    spacing: 6

    // Only data:image URLs the backend made. Anything else shows nothing.
    function safe(u) { return /^data:image\/(png|jpeg);base64,[A-Za-z0-9+\/=]+$/.test(u || "") ? u : "" }

    // index -> {state: "" | loading | ready | failed, detail, full}
    property var states: ({})
    function put(i, v) { var s = Object.assign({}, states); s[i] = v; states = s }
    function setState(i, st, detail) {
        if (st !== "ready") { put(i, { state: st, detail: detail || "", full: "" }); return }
        var asked = post.id
        app.call(app.backend.fullImage(post.id, i), function (url) {
            if (!strip.post || strip.post.id !== asked) return
            var u = strip.safe(url)
            strip.put(i, u !== "" ? { state: "ready", detail: "", full: u } : { state: "failed", detail: "not shown", full: "" })
        })
    }
    Connections {
        target: strip.app.backend
        ignoreUnknownSignals: true
        function onImageState(postId, index, state, detail) {
            if (strip.post && postId === strip.post.id) strip.setState(index, state, detail)
        }
    }
    onPostChanged: {
        // Images loaded before, kept on this machine: shown again without asking the network.
        if (!post || !post.images) return
        for (var i = 0; i < post.images.length; i++)
            if (post.images[i].cached && !(states[i] && states[i].state === "ready")) app.call(app.backend.loadImage(post.id, i))
    }

    Flow {
        Layout.fillWidth: true
        spacing: 8
        Repeater {
            model: strip.images
            delegate: Rectangle {
                id: cell
                readonly property var st: strip.states[index] || ({ state: "" })
                readonly property bool open: st.state === "ready" && st.full !== ""
                readonly property real fullW: Math.min(strip.width, 900)
                width: open ? fullW : 168
                height: open ? Math.max(40, fullW * modelData.h / Math.max(1, modelData.w)) : 168 + 34
                radius: 6; color: strip.app.bg; border.color: strip.app.line
                Image {
                    id: pic
                    anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 4
                    width: parent.width - 8; height: cell.open ? parent.height - 8 : 160
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true; cache: false
                    source: cell.open ? cell.st.full : strip.safe(modelData.thumb)
                    Accessible.name: cell.open ? "Image " + (index + 1) : "Thumbnail " + (index + 1)
                }
                Text {
                    anchors.centerIn: pic
                    visible: pic.status !== Image.Ready
                    text: pic.status === Image.Error ? "thumbnail unreadable" : ""
                    color: strip.app.dim; font.pixelSize: 11; textFormat: Text.PlainText
                }
                // Under the thumbnail: what a click does, or how it went.
                Text {
                    visible: !cell.open
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 6
                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 6
                    horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                    textFormat: Text.PlainText; font.pixelSize: 12
                    color: cell.st.state === "failed" ? strip.app.warn : strip.app.link
                    text: cell.st.state === "loading" ? "Loading…"
                        : cell.st.state === "failed" ? "Not loaded"
                        : "Load image · " + Math.max(1, Math.round(modelData.bytes / 1024)) + " KB"
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !cell.open && cell.st.state !== "loading"
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    Accessible.role: Accessible.Button
                    Accessible.name: "Load image " + (index + 1)
                    onClicked: strip.app.call(strip.app.backend.loadImage(strip.post.id, index), function (r) {
                        if (r !== "") strip.setState(index, "failed", r.replace(/^error: /, ""))
                    })
                }
            }
        }
    }
    // The one-line disclosure, before anything is fetched.
    Text {
        Layout.fillWidth: true
        visible: strip.images.length > 0
        textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.pixelSize: 11; color: strip.app.dim
        text: "Loading an image fetches it from Logos Storage: the node that serves it sees your node’s address. It is shown only if it matches the signed hash."
    }
    Repeater {
        model: strip.images
        delegate: Text {
            readonly property var st: strip.states[index] || ({ state: "" })
            visible: st.state === "failed"
            Layout.fillWidth: true
            textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.pixelSize: 12; color: strip.app.warn
            text: "Image " + (index + 1) + ": " + (st.detail || "not loaded")
        }
    }
    Text {
        Layout.fillWidth: true
        visible: strip.missing > 0
        textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.pixelSize: 12; color: strip.app.dim
        text: strip.missing + (strip.missing === 1 ? " image" : " images") + " not available yet: this copy of the post came without its image list."
    }
}
