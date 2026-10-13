import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Which forum is shown: the public Logos Forum or a private one joined from
// an invite, and what can be done with private forums. `app` is Main.qml's
// root: its palette, its backend and its call() helper.
Flow {
    id: sw
    property var app
    property var dialogs           // PrivateForums.qml
    spacing: 8

    component SwButton: Button {
        id: b
        HoverHandler { cursorShape: b.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        contentItem: Text { text: b.text; font: b.font; color: b.enabled ? sw.app.text : sw.app.dim
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { implicitWidth: 80; implicitHeight: 40; radius: 6
                                color: b.down ? sw.app.line : sw.app.raised
                                border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.06) }
    }

    ComboBox {
        id: box
        objectName: "forumBox"
        width: sw.app.compact ? Math.max(120, sw.width - more.implicitWidth - sw.spacing) : Math.min(sw.width, 300)
        model: sw.app.forums.map(function (f) { return f.private ? f.name + "  · private" : f.name })
        currentIndex: sw.app.forums.findIndex(function (f) { return f.current })
        onActivated: function (i) { sw.app.call(sw.app.backend.selectForum(sw.app.forums[i].id), sw.app.plainOrError) }
        palette: sw.app.palette
        popup.palette: sw.app.palette
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle { implicitHeight: 40; radius: 6; color: box.down ? sw.app.line : sw.app.raised
                                border.width: 1; border.color: sw.app.isPrivate ? sw.app.accent : Qt.rgba(1, 1, 1, 0.06) }
        contentItem: Text {
            leftPadding: 10; rightPadding: box.indicator.width + 6
            text: box.displayText; textFormat: Text.PlainText; elide: Text.ElideRight
            color: sw.app.text; verticalAlignment: Text.AlignVCenter; font.pixelSize: 14
        }
    }
    // Wide: every action at hand. Compact: one button and a menu, so the
    // switcher keeps to one line on a phone.
    SwButton { visible: !sw.app.compact; text: "Create private forum"; onClicked: sw.dialogs.openCreate() }
    SwButton { visible: !sw.app.compact; text: "Join with invite"; onClicked: sw.dialogs.openJoin() }
    SwButton { visible: !sw.app.compact && sw.app.isPrivate; text: "Copy invite"; onClicked: sw.dialogs.openInvite(sw.app.currentForum) }
    SwButton { visible: !sw.app.compact && sw.app.isPrivate; text: "Leave"; onClicked: sw.dialogs.openLeave(sw.app.currentForum, sw.app.currentForumName) }
    SwButton { visible: !sw.app.compact; text: "Muted…"; onClicked: sw.dialogs.openMuted() }
    SwButton {
        id: more
        visible: sw.app.compact
        text: "Forums…"
        onClicked: menu.open()
        Menu {
            id: menu
            y: more.height
            palette: sw.app.palette
            MenuItem { text: "Create private forum"; onTriggered: sw.dialogs.openCreate() }
            MenuItem { text: "Join with invite"; onTriggered: sw.dialogs.openJoin() }
            MenuItem { text: "Copy invite"; enabled: sw.app.isPrivate; height: enabled ? implicitHeight : 0; visible: enabled
                       onTriggered: sw.dialogs.openInvite(sw.app.currentForum) }
            MenuItem { text: "Leave this forum"; enabled: sw.app.isPrivate; height: enabled ? implicitHeight : 0; visible: enabled
                       onTriggered: sw.dialogs.openLeave(sw.app.currentForum, sw.app.currentForumName) }
            MenuItem { text: "Muted and hidden…"; onTriggered: sw.dialogs.openMuted() }
        }
    }
}
