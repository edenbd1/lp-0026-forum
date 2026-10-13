import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown while the account keys are sealed with a passphrase and not yet
// unlocked this session. Reading, and posting anonymously, never wait for it.
Rectangle {
    id: banner
    property var app
    color: app.raised; radius: 6; border.color: app.warn
    implicitHeight: col.implicitHeight + 20
    function submit() {
        if (pass.text === "") return
        busy = true
        var p = pass.text
        pass.text = ""
        app.call(app.backend.unlock(p), function (r) { banner.busy = false; banner.error = r })
    }
    property bool busy: false
    property string error: ""
    ColumnLayout {
        id: col
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10
        spacing: 6
        Text { Layout.fillWidth: true; text: "Your keys are locked"; font.bold: true; font.pixelSize: 14; color: banner.app.warn
               textFormat: Text.PlainText; wrapMode: Text.WrapAtWordBoundaryOrAnywhere }
        Text { Layout.fillWidth: true; color: banner.app.dim; font.pixelSize: 13; textFormat: Text.PlainText; wrapMode: Text.WrapAtWordBoundaryOrAnywhere
               text: "Enter your passphrase to post as one of your accounts. You can read, and post anonymously, without it." }
        Flow {
            Layout.fillWidth: true; spacing: 8
            TextField {
                id: pass
                objectName: "unlockField"
                width: Math.min(320, col.width); height: 40
                echoMode: TextInput.Password; placeholderText: "Passphrase"
                color: banner.app.text; placeholderTextColor: banner.app.dim
                background: Rectangle { radius: 6; color: banner.app.bg; border.color: pass.activeFocus ? banner.app.focusRing : banner.app.line }
                onAccepted: banner.submit()
            }
            Button {
                id: go
                text: banner.busy ? "Unlocking…" : "Unlock"; enabled: !banner.busy && pass.text !== ""
                height: 40
                onClicked: banner.submit()
                contentItem: Text { text: go.text; color: go.enabled ? banner.app.text : banner.app.dim; font.pixelSize: 14
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { implicitWidth: 90; radius: 6; color: go.down ? banner.app.line : banner.app.panel; border.color: banner.app.line }
            }
        }
        Text { visible: banner.error !== ""; text: banner.error; color: "#e5484d"; font.pixelSize: 13; textFormat: Text.PlainText }
    }
}
