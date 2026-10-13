import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Images attached to the post being written: picked from a file dialog (the
// platform's own, or Qt's; never a shell command), re-encoded and stored by
// the backend, shown here as the thumbnails readers will see.
ColumnLayout {
    id: att
    property var app
    property bool anonymous: false     // images are not offered for anonymous posts
    readonly property int maxImages: 4
    // [{token, thumb, state: uploading | ready | failed, detail}]
    property var items: []
    readonly property var tokens: items.filter(function (i) { return i.state !== "failed" }).map(function (i) { return i.token })
    readonly property bool busy: items.some(function (i) { return i.state === "uploading" })
    readonly property bool failed: items.some(function (i) { return i.state === "failed" })
    spacing: 6

    function clear(discard) {
        if (discard) for (var i = 0; i < items.length; i++) app.call(app.backend.discardImage(items[i].token))
        items = []
    }
    function remove(token) {
        app.call(app.backend.discardImage(token))
        items = items.filter(function (i) { return i.token !== token })
    }
    function add(url) {
        if (items.length >= maxImages) { app.lastError = "At most " + maxImages + " images per post."; return }
        app.call(app.backend.attachImage(url), function (r) {
            if (r.indexOf("error: ") === 0) { app.lastError = r.substring(7); return }
            try {
                var o = JSON.parse(r)
                att.items = att.items.concat([{ token: o.token, thumb: o.thumb, state: "uploading", detail: "", w: o.w, h: o.h, bytes: o.bytes }])
                app.lastError = ""
            } catch (e) { app.lastError = "The image could not be attached." }
        })
    }
    Connections {
        target: att.app.backend
        ignoreUnknownSignals: true
        function onImageUploaded(token, state, detail) {
            att.items = att.items.map(function (i) {
                return i.token === token ? Object.assign({}, i, { state: state, detail: detail }) : i
            })
        }
    }

    // The dialog lives in its own file, loaded on demand: if this Basecamp's
    // QML has no QtQuick.Dialogs, the rest of the view still works.
    Loader { id: picker; active: false; source: "ImagePicker.qml"
             onLoaded: { item.picked.connect(function (urls) { for (var i = 0; i < urls.length; i++) att.add(urls[i]) }); item.open() } }

    RowLayout {
        Layout.fillWidth: true; spacing: 8
        Button {
            id: addButton
            text: "Add image"
            enabled: !att.anonymous && att.items.length < att.maxImages
            onClicked: {
                if (picker.status === Loader.Ready) picker.item.open()
                else picker.active = true
                if (picker.status === Loader.Error) att.app.lastError = "No file dialog is available in this Basecamp."
            }
            HoverHandler { cursorShape: addButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
            contentItem: Text { text: "🖼 " + addButton.text; color: addButton.enabled ? att.app.text : att.app.dim
                                font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { implicitWidth: 110; implicitHeight: 32; radius: 6; color: addButton.down ? att.app.line : att.app.raised
                                    border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.06) }
        }
        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.pixelSize: 11; color: att.app.dim
            text: att.anonymous
                ? "No images in anonymous posts: your own Storage node serves them, which would link the post to you."
                : "Up to 4. Location, camera and every other metadata are removed. Your node serves the images: readers who load one connect to it."
        }
    }
    Flow {
        Layout.fillWidth: true
        visible: att.items.length > 0
        spacing: 8
        Repeater {
            model: att.items
            delegate: Rectangle {
                width: 84; height: 84; radius: 6; color: att.app.bg
                border.color: modelData.state === "failed" ? att.app.warn : att.app.line
                Image {
                    anchors.fill: parent; anchors.margins: 3
                    fillMode: Image.PreserveAspectCrop; asynchronous: true; cache: false
                    source: /^data:image\/(png|jpeg);base64,[A-Za-z0-9+\/=]+$/.test(modelData.thumb || "") ? modelData.thumb : ""
                    opacity: modelData.state === "ready" ? 1 : 0.5
                }
                Text {
                    anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottomMargin: 4
                    visible: modelData.state !== "ready"
                    text: modelData.state === "uploading" ? "storing…" : "failed"
                    color: modelData.state === "failed" ? att.app.warn : att.app.text
                    font.pixelSize: 10; textFormat: Text.PlainText
                    style: Text.Outline; styleColor: "#000000"
                }
                Rectangle {
                    anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 3
                    width: 20; height: 20; radius: 10; color: "#000000"; opacity: 0.75
                    Text { anchors.centerIn: parent; text: "✕"; color: "white"; font.pixelSize: 11 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                Accessible.role: Accessible.Button; Accessible.name: "Remove image"
                                onClicked: att.remove(modelData.token) }
                }
            }
        }
    }
}
