import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Dialogs for private forums (create, join, invite, leave) and for what the
// reader has muted or hidden. Fills Main.qml's root so the dialogs centre on
// it. `app` is Main.qml's root.
Item {
    id: pf
    property var app
    anchors.fill: parent

    property string inviteFor: ""
    property string invite: ""
    property string leaveId: ""
    property string leaveName: ""
    property string error: ""
    property var moderation: ({ muted: [], hidden: [] })

    function openCreate() { error = ""; groupName.text = ""; createDialog.open() }
    function openJoin() { error = ""; inviteField.text = ""; joinDialog.open() }
    function openInvite(id) {
        inviteFor = id; error = ""
        app.call(app.backend.groupInvite(id), function (r) {
            if (r.indexOf("error: ") === 0) { app.lastError = r.substring(7); return }
            pf.invite = r; inviteDialog.open()
        })
    }
    function openLeave(id, name) { leaveId = id; leaveName = name; leaveDialog.open() }
    function refreshModeration() {
        app.call(app.backend.moderationList(), function (r) {
            try { var m = JSON.parse(r); pf.moderation = { muted: m.muted || [], hidden: m.hidden || [] } } catch (e) {}
        })
    }
    function openMuted() { refreshModeration(); mutedDialog.open() }

    component PfButton: Button {
        id: b
        property bool accent: false
        HoverHandler { cursorShape: b.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        contentItem: Text { text: b.text; font: b.font; color: b.enabled ? (b.accent ? "#ffffff" : pf.app.text) : pf.app.dim
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { implicitWidth: 80; implicitHeight: 40; radius: 6
                                color: b.accent && b.enabled ? pf.app.accentStrong : (b.down ? pf.app.line : pf.app.raised)
                                border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.06) }
    }
    component PfText: Text { Layout.fillWidth: true; color: pf.app.dim; font.pixelSize: 13
                             wrapMode: Text.WrapAtWordBoundaryOrAnywhere; textFormat: Text.PlainText }
    component PfDialog: Dialog {
        id: d
        parent: pf
        palette: pf.app.palette
        modal: true; anchors.centerIn: parent
        width: Math.min(560, pf.width - 24)
        height: Math.min(implicitHeight, pf.height - 24)
        background: Rectangle { color: pf.app.panel; radius: 8; border.color: pf.app.line }
        header: Text { text: d.title; color: pf.app.text; font.pixelSize: 16; font.bold: true; padding: 16
                       wrapMode: Text.WrapAtWordBoundaryOrAnywhere; textFormat: Text.PlainText }
        footer: Item { implicitHeight: 12 }
    }
    component PfField: TextField {
        id: f
        Layout.fillWidth: true
        color: pf.app.text; placeholderTextColor: pf.app.dim
        background: Rectangle { implicitHeight: 40; radius: 6; color: pf.app.bg; border.color: f.activeFocus ? pf.app.focusRing : pf.app.line }
    }

    PfDialog {
        id: createDialog
        objectName: "createDialog"
        title: "Create a private forum"
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            PfText { text: "Only people you give the invite to can read it or post in it. Posts are signed as usual and sealed before they leave this device, on a topic public forum nodes never listen to." }
            PfField { id: groupName; placeholderText: "Forum name"; maximumLength: 64 }
            Text { visible: pf.error !== ""; text: pf.error; color: pf.app.warn; font.pixelSize: 13; textFormat: Text.PlainText
                   Layout.fillWidth: true; wrapMode: Text.WrapAtWordBoundaryOrAnywhere }
            RowLayout {
                Layout.alignment: Qt.AlignRight; spacing: 10
                PfButton { text: "Cancel"; onClicked: createDialog.close() }
                PfButton {
                    text: "Create"; accent: true; enabled: groupName.text.trim().length > 0
                    onClicked: pf.app.call(pf.app.backend.createGroup(groupName.text), function (r) {
                        if (r.indexOf("error: ") === 0) { pf.error = r.substring(7); return }
                        createDialog.close()
                        pf.invite = r; pf.inviteFor = pf.app.currentForum
                        inviteDialog.open()
                    })
                }
            }
        }
    }

    PfDialog {
        id: joinDialog
        objectName: "joinDialog"
        title: "Join with an invite"
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            PfText { text: "Paste the invite you were given. It starts with logosforum://join/. Anyone holding it can read the forum, so keep it among its members." }
            PfField { id: inviteField; placeholderText: "logosforum://join/…" }
            Text { visible: pf.error !== ""; text: pf.error; color: pf.app.warn; font.pixelSize: 13; textFormat: Text.PlainText
                   Layout.fillWidth: true; wrapMode: Text.WrapAtWordBoundaryOrAnywhere }
            RowLayout {
                Layout.alignment: Qt.AlignRight; spacing: 10
                PfButton { text: "Cancel"; onClicked: joinDialog.close() }
                PfButton {
                    text: "Join"; accent: true; enabled: inviteField.text.trim().length > 0
                    onClicked: pf.app.call(pf.app.backend.joinGroup(inviteField.text), function (r) {
                        if (r.indexOf("error: ") === 0) { pf.error = r.substring(7); return }
                        inviteField.text = ""
                        joinDialog.close()
                    })
                }
            }
        }
    }

    PfDialog {
        id: inviteDialog
        objectName: "inviteDialog"
        title: "Invite to " + pf.app.forumNameOf(pf.inviteFor)
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            PfText { text: "Whoever has this invite can read every post of this forum and post in it, for as long as it exists. Share it only with the people you mean, over a channel you trust. It cannot be withdrawn: to remove someone, create a new forum and invite the others again." }
            Rectangle {
                Layout.fillWidth: true; color: pf.app.bg; radius: 6; border.color: pf.app.line
                implicitHeight: inviteText.implicitHeight + 16
                TextEdit {
                    id: inviteText
                    anchors.fill: parent; anchors.margins: 8
                    text: pf.invite
                    readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText
                    color: pf.app.text; selectionColor: pf.app.accentStrong
                    font.family: "monospace"; font.pixelSize: 12
                    wrapMode: TextEdit.WrapAnywhere
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight; spacing: 10
                Text { id: copied; visible: false; text: "Copied"; color: pf.app.ok; font.pixelSize: 13 }
                PfButton { text: "Copy"; accent: true
                           onClicked: { inviteText.selectAll(); inviteText.copy(); inviteText.deselect(); copied.visible = true } }
                PfButton { text: "Close"; onClicked: { copied.visible = false; inviteDialog.close() } }
            }
        }
    }

    PfDialog {
        id: leaveDialog
        objectName: "leaveDialog"
        title: "Leave " + pf.leaveName + "?"
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            PfText { text: "Its invite and every post of it are deleted from this device. The other members keep theirs, and you can join again with the invite." }
            RowLayout {
                Layout.alignment: Qt.AlignRight; spacing: 10
                PfButton { text: "Cancel"; onClicked: leaveDialog.close() }
                PfButton { text: "Leave"; accent: true
                           onClicked: pf.app.call(pf.app.backend.leaveGroup(pf.leaveId), function (r) { pf.app.plainOrError(r); leaveDialog.close() }) }
            }
        }
    }

    PfDialog {
        id: mutedDialog
        objectName: "mutedDialog"
        title: "Muted and hidden"
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            PfText { text: "Only for " + (pf.app.myLabel || "this account") + ", on this device. Nothing here is sent to anyone, and the posts are still kept and passed on to others: they are just not shown to you. Muting is by key, never by name, so someone using the same alias is not muted." }
            Text { text: "Muted keys"; color: pf.app.text; font.bold: true; font.pixelSize: 14 }
            PfText { visible: pf.moderation.muted.length === 0; text: "Nobody." }
            Repeater {
                model: pf.moderation.muted
                delegate: RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Text { Layout.fillWidth: true; text: pf.app.shortKey(modelData.key) + " · since " + pf.app.when(modelData.at)
                           color: pf.app.text; font.family: "monospace"; font.pixelSize: 12; elide: Text.ElideRight
                           ToolTip.visible: kh.hovered; ToolTip.text: modelData.key; HoverHandler { id: kh } }
                    PfButton { text: "Unmute"; onClicked: pf.app.call(pf.app.backend.unmuteAuthor(modelData.key), function () { pf.refreshModeration() }) }
                }
            }
            Text { text: "Hidden posts"; color: pf.app.text; font.bold: true; font.pixelSize: 14 }
            PfText { visible: pf.moderation.hidden.length === 0; text: "None." }
            Repeater {
                model: pf.moderation.hidden
                delegate: RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Text { Layout.fillWidth: true; text: modelData.title !== "" ? modelData.title : modelData.id.substring(0, 12) + "…"
                           color: pf.app.text; font.pixelSize: 13; elide: Text.ElideRight; textFormat: Text.PlainText }
                    PfButton { text: "Show again"; onClicked: pf.app.call(pf.app.backend.unhidePost(modelData.id), function () { pf.refreshModeration() }) }
                }
            }
            PfButton { Layout.alignment: Qt.AlignRight; text: "Close"; onClicked: mutedDialog.close() }
        }
    }
}
