import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The fixed set of reactions under a post, with how many keys chose each.
// A click toggles ours: a signed reaction from the selected account, or a
// signed withdrawal of it.
Flow {
    id: bar
    property var app
    property var post
    readonly property var emoji: ["👍", "❤️", "🎉", "👀", "✅"]
    readonly property var names: ["thumbs up", "heart", "party", "eyes", "done"]
    readonly property var counts: post && post.reactions ? post.reactions : [0, 0, 0, 0, 0]
    readonly property var mine: post && post.reacted ? post.reacted : [false, false, false, false, false]
    property bool expanded: false
    readonly property bool any: counts.some(function (c) { return c > 0 })
    spacing: 6
    visible: post !== null && post !== undefined

    Repeater {
        model: 5
        delegate: Rectangle {
            id: chip
            readonly property int n: bar.counts[index] || 0
            readonly property bool on: bar.mine[index] === true
            // Reactions someone used, always; the rest when the reader opens the picker.
            visible: n > 0 || bar.expanded
            radius: 12; height: 26
            width: label.implicitWidth + 16
            color: on ? Qt.rgba(0.89, 0.33, 0.17, 0.25) : bar.app.raised
            border.color: on ? bar.app.accent : bar.app.line
            Text {
                id: label
                anchors.centerIn: parent
                textFormat: Text.PlainText; font.pixelSize: 13; color: bar.app.text
                text: bar.emoji[index] + (chip.n > 0 ? " " + chip.n : "")
            }
            MouseArea {
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                Accessible.role: Accessible.Button
                Accessible.name: (chip.on ? "Remove reaction " : "React ") + bar.names[index]
                onClicked: {
                    bar.expanded = false
                    bar.app.call(bar.app.backend.react(bar.post.id, index), function (r) {
                        if (r !== "") bar.app.lastError = r.replace(/^error: /, "")
                        else bar.app.refreshThread()
                    })
                }
            }
            ToolTip.visible: hov.hovered; ToolTip.delay: 500
            ToolTip.text: chip.on ? "You reacted. Click to withdraw it (signed by your account)." : "React, signed by your account"
            HoverHandler { id: hov }
        }
    }
    Rectangle {
        visible: !bar.expanded
        radius: 12; height: 26; width: plus.implicitWidth + 16
        color: "transparent"; border.color: bar.app.line
        Text { id: plus; anchors.centerIn: parent; text: bar.any ? "+" : "☺ +"; color: bar.app.dim; font.pixelSize: 13; textFormat: Text.PlainText }
        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            Accessible.role: Accessible.Button; Accessible.name: "Add a reaction"
            onClicked: bar.expanded = true
        }
    }
}
