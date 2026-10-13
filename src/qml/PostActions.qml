import QtQuick
import QtQuick.Controls

// "hide" and "mute" next to someone else's post. Muting is by the key the
// signature was checked against, never by the name shown; both stay on this
// device, for the selected account. `app` is Main.qml's root.
Row {
    id: pa
    property var app
    property var post
    // A topic hidden from its own thread: the view goes back to the list.
    signal hidden()
    spacing: 8
    visible: post !== null && post !== undefined && !post.mine

    component Act: Text {
        id: a
        signal clicked()
        color: ah.hovered ? pa.app.text : pa.app.dim; font.pixelSize: 11
        textFormat: Text.PlainText
        HoverHandler { id: ah; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: a.clicked() }
    }
    Act {
        text: "hide"
        Accessible.role: Accessible.Button; Accessible.name: "Hide this post"
        onClicked: pa.app.call(pa.app.backend.hidePost(pa.post.id), function (r) { if (pa.app.plainOrError(r)) pa.hidden() })
    }
    Act {
        // An anonymous key signs one post: muting it hides that post alone.
        text: pa.post && pa.post.mode === "anonymous" ? "mute this key" : "mute author"
        Accessible.role: Accessible.Button; Accessible.name: "Mute the key " + (pa.post ? pa.post.authorKey : "")
        onClicked: pa.app.call(pa.app.backend.muteAuthor(pa.post.authorKey), function (r) { if (pa.app.plainOrError(r)) pa.hidden() })
    }
}
