import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// In the Accounts dialog: the passphrase that seals the account keys, the
// recovery phrase of an account, and restoring one from its words.
ColumnLayout {
    id: safety
    property var app
    spacing: 10
    readonly property bool sealed: app.keys.protected === true
    readonly property bool unlocked: app.keys.unlocked === true
    property string message: ""
    property bool busy: false

    component Btn: Button {
        id: b
        HoverHandler { cursorShape: b.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        contentItem: Text { text: b.text; font.pixelSize: 14; color: b.enabled ? safety.app.text : safety.app.dim
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { implicitWidth: 80; implicitHeight: 40; radius: 6
                                color: b.down ? safety.app.line : safety.app.raised; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.06) }
    }
    component Field: TextField {
        id: f
        color: safety.app.text; placeholderTextColor: safety.app.dim
        background: Rectangle { implicitHeight: 40; radius: 6; color: safety.app.bg; border.color: f.activeFocus ? safety.app.focusRing : safety.app.line }
    }
    component Note: Text { color: safety.app.dim; font.pixelSize: 12; wrapMode: Text.WrapAtWordBoundaryOrAnywhere; textFormat: Text.PlainText }

    function done(r) {
        busy = false
        message = r === "" ? "Done." : r
        current.text = ""; next.text = ""; again.text = ""
    }
    function run(pending) { busy = true; message = "Working… (deriving the key takes a second)"; app.call(pending, done) }

    // ── Passphrase ──
    Text { text: "Passphrase"; color: safety.app.text; font.pixelSize: 14; font.bold: true }
    Note {
        Layout.fillWidth: true
        text: !safety.sealed
              ? "Your keys are stored unencrypted in forum.db, which only your user can read. A passphrase seals them (Argon2id and XChaCha20-Poly1305); you then unlock them once per session. If you forget it, only your recovery phrases can bring the accounts back."
              : safety.unlocked ? "Your keys are sealed with your passphrase, and unlocked for this session."
                                : "Your keys are sealed with your passphrase, and locked."
    }
    Flow {
        Layout.fillWidth: true; spacing: 8
        Field { id: current; visible: safety.sealed; width: 220; echoMode: TextInput.Password; placeholderText: "Current passphrase" }
        Field { id: next; width: 220; echoMode: TextInput.Password; placeholderText: safety.sealed ? "New passphrase (empty: remove)" : "New passphrase" }
        Field { id: again; width: 220; echoMode: TextInput.Password; placeholderText: "New passphrase again" }
        Btn {
            text: !safety.sealed ? "Set passphrase" : next.text === "" ? "Remove passphrase" : "Change passphrase"
            enabled: !safety.busy && next.text === again.text && (safety.sealed ? current.text !== "" : next.text !== "")
            onClicked: safety.run(safety.app.backend.setPassphrase(current.text, next.text))
        }
        Btn { visible: safety.sealed && safety.unlocked; text: "Lock now"; enabled: !safety.busy
              onClicked: safety.app.call(safety.app.backend.lockKeys(), safety.done) }
    }
    Note { Layout.fillWidth: true; visible: next.text !== again.text && again.text !== ""; text: "The two new passphrases differ."; color: safety.app.warn }
    Note { Layout.fillWidth: true; visible: safety.message !== ""; text: safety.message; color: safety.message === "Done." ? safety.app.ok : safety.app.warn }

    Rectangle { Layout.fillWidth: true; height: 1; color: safety.app.line }

    // ── Restore ──
    Text { text: "Restore an account"; color: safety.app.text; font.pixelSize: 14; font.bold: true }
    Note {
        Layout.fillWidth: true
        text: "Type the 24 words of an account's recovery phrase. It comes back with the same key. Anonymous posts never come back: their keys were made for one post each and wiped."
    }
    TextArea {
        id: words
        objectName: "restoreWords"
        Layout.fillWidth: true; Layout.preferredHeight: 70
        placeholderText: "24 words"; wrapMode: TextArea.Wrap
        color: safety.app.text; placeholderTextColor: safety.app.dim
        background: Rectangle { radius: 6; color: safety.app.bg; border.color: words.activeFocus ? safety.app.focusRing : safety.app.line }
    }
    Flow {
        Layout.fillWidth: true; spacing: 8
        Field { id: restoreLabel; width: 220; placeholderText: "Account name (optional)" }
        Btn {
            text: "Restore"; enabled: words.text.trim() !== ""
            onClicked: {
                var w = words.text
                words.text = ""
                safety.app.call(safety.app.backend.restoreAccount(restoreLabel.text, w), function (r) {
                    safety.message = r === "" ? "Restored." : r
                    if (r === "") restoreLabel.text = ""
                })
            }
        }
    }

    // ── The words of one account ──
    function showPhrase(label) {
        phrase.label = label; phrase.words = []; phrase.error = ""
        phrase.open()
        if (!sealed) phrase.fetch("")
    }
    Dialog {
        id: phrase
        property string label: ""
        property var words: []
        property string error: ""
        function fetch(pass) {
            safety.app.call(safety.app.backend.recoveryPhrase(label, pass), function (r) {
                if (r.indexOf("error: ") === 0) { phrase.error = r.substring(7); return }
                phrase.words = r.split(" ")
            })
        }
        onClosed: { words = []; askPass.text = "" }   // the words leave the view when it closes
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(560, (parent ? parent.width : 560) - 24)
        palette: safety.app.palette
        background: Rectangle { color: safety.app.panel; radius: 8; border.color: safety.app.line }
        header: Text { text: "Recovery phrase · " + phrase.label; color: safety.app.text; font.pixelSize: 16; font.bold: true; padding: 16
                       elide: Text.ElideRight; textFormat: Text.PlainText }
        footer: Item { implicitHeight: 56
            Btn { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter
                  text: phrase.words.length ? "I wrote them down" : "Close"; onClicked: phrase.close() } }
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            Note { Layout.fillWidth: true; visible: phrase.words.length === 0 && safety.sealed; text: "Enter your passphrase to see the words." }
            RowLayout {
                visible: phrase.words.length === 0 && safety.sealed
                Field { id: askPass; Layout.preferredWidth: 240; echoMode: TextInput.Password; placeholderText: "Passphrase"
                        onAccepted: { var p = text; text = ""; phrase.fetch(p) } }
                Btn { text: "Show"; enabled: askPass.text !== ""; onClicked: { var p = askPass.text; askPass.text = ""; phrase.fetch(p) } }
            }
            Note { Layout.fillWidth: true; visible: phrase.error !== ""; text: phrase.error; color: safety.app.warn }
            Note {
                Layout.fillWidth: true; visible: phrase.words.length > 0
                text: "Write these words down, in order, and keep them offline. Anyone holding them can post as this account. They are the key of today: after a rotation, you will need new words."
            }
            GridLayout {
                visible: phrase.words.length > 0
                Layout.fillWidth: true
                columns: phrase.width < 420 ? 2 : 3
                columnSpacing: 16; rowSpacing: 6
                Repeater {
                    model: phrase.words
                    delegate: Text {
                        Layout.fillWidth: true
                        text: (index + 1) + ". " + modelData
                        color: safety.app.text; font.pixelSize: 14; font.family: "monospace"; textFormat: Text.PlainText
                    }
                }
            }
        }
    }
}
