import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Replies to you, and your posts, for the account in use. Matched by its key,
// never by a name; anonymous posts are nobody's here (see core/include/forum/inbox.h).
Dialog {
    id: box
    property var app
    property var items: []
    property var mine: []
    property string handle: ""
    property int tab: 0
    title: "Inbox"
    modal: true
    anchors.centerIn: parent
    width: Math.min(720, (parent ? parent.width : 720) - 24)
    height: Math.min(640, (parent ? parent.height : 640) - 24)
    palette: app.palette
    background: Rectangle { color: box.app.panel; radius: 8; border.color: box.app.line }
    header: Text { text: box.title + " · " + box.app.myLabel; color: box.app.text; font.pixelSize: 16; font.bold: true; padding: 16
                   elide: Text.ElideRight; textFormat: Text.PlainText }

    function load() {
        app.call(app.backend.inbox(), function (json) {
            try { var v = JSON.parse(json); box.items = v.items || []; box.handle = v.handle || "" } catch (e) { box.items = [] }
        })
        app.call(app.backend.myPosts(), function (json) { try { box.mine = JSON.parse(json) } catch (e) { box.mine = [] } })
    }
    onOpened: load()
    Connections { target: box.app; function onInboxUnreadChanged() { if (box.opened) box.load() } }

    component Btn: Button {
        id: b
        HoverHandler { cursorShape: b.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        contentItem: Text { text: b.text; font.pixelSize: 14; color: b.enabled ? box.app.text : box.app.dim
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { implicitWidth: 80; implicitHeight: 36; radius: 6
                                color: b.down || b.checked ? box.app.line : box.app.raised; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.06) }
    }

    footer: Item {
        implicitHeight: 56
        Btn { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter
              text: "Close"; onClicked: box.close() }
    }

    ColumnLayout {
        anchors.fill: parent; spacing: 10
        Flow {
            Layout.fillWidth: true; spacing: 8
            Btn { objectName: "inboxTab"; text: "Replies to you" + (box.app.inboxUnread > 0 ? " (" + box.app.inboxUnread + ")" : ""); checkable: true; checked: box.tab === 0; onClicked: box.tab = 0 }
            Btn { text: "Your posts"; checkable: true; checked: box.tab === 1; onClicked: box.tab = 1 }
            Btn { visible: box.tab === 0; text: "Mark all read"; enabled: box.app.inboxUnread > 0
                  onClicked: box.app.call(box.app.backend.markInboxRead(), function () { box.load() }) }
        }
        // How others mention this account: its key, not its name.
        RowLayout {
            Layout.fillWidth: true; visible: box.tab === 0 && box.handle !== ""
            spacing: 8
            Text { text: "Mention me as"; color: box.app.dim; font.pixelSize: 12 }
            TextEdit {
                id: handleField
                Layout.fillWidth: true
                text: "@" + box.handle; readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText
                color: box.app.accent; selectionColor: box.app.accentStrong; font.family: "monospace"; font.pixelSize: 12
                wrapMode: TextEdit.WrapAnywhere
            }
            Btn { text: "Copy"; onClicked: { handleField.selectAll(); handleField.copy(); handleField.deselect() } }
        }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; spacing: 4
            model: box.tab === 0 ? box.items : box.mine
            delegate: Rectangle {
                width: ListView.view.width
                height: row.implicitHeight + 14
                radius: 4
                color: hover.hovered ? box.app.line : "transparent"
                HoverHandler { id: hover }
                Accessible.role: Accessible.Button
                Accessible.name: modelData.topicTitle || ""
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { box.app.openThread(modelData.topicId); box.close() } }
                ColumnLayout {
                    id: row
                    anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.margins: 8
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        Rectangle { visible: modelData.unread === true; width: 7; height: 7; radius: 4; color: box.app.accent }
                        Text {
                            Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText
                            color: box.app.text; font.pixelSize: 13; font.bold: modelData.unread === true
                            text: (box.tab === 0
                                   ? (modelData.why === "mention" ? "Mentioned you" : modelData.why === "topic" ? "Replied to your topic" : "Replied after you")
                                   : (modelData.title ? "Your topic" : modelData.mode === "alias" ? "Your reply, as an alias" : "Your reply"))
                                  + " · " + (modelData.topicTitle || "(topic not received yet)")
                        }
                    }
                    Text {
                        Layout.fillWidth: true; maximumLineCount: 2; elide: Text.ElideRight; wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        textFormat: Text.PlainText; color: box.app.dim; font.pixelSize: 12
                        text: (modelData.body || "").replace(/\s+/g, " ")
                    }
                    Text {
                        textFormat: Text.PlainText; color: box.app.dim; font.pixelSize: 11
                        text: (box.tab === 0 ? box.app.listAuthor(modelData) + " · " : "") + box.app.when(modelData.ts)
                    }
                }
            }
            Text {
                anchors.centerIn: parent; width: parent.width - 32; horizontalAlignment: Text.AlignHCenter
                visible: list.count === 0; color: box.app.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                text: box.tab === 0
                      ? "Nothing yet. Replies to your topics, replies after yours, and posts that mention your key show up here. Anonymous posts are not counted as yours: the forum keeps no note of them."
                      : "Nothing posted as " + box.app.myLabel + " yet. Anonymous posts are never listed here."
            }
        }
    }
}
