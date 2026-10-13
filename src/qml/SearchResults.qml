import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Full-text search over the posts stored on this machine (titles and bodies,
// every one of them signature-checked when it arrived). Nothing leaves the
// machine: the query runs in the local SQLite index.
ListView {
    id: results
    property var app
    property string query: ""
    property var hits: []
    property bool searched: false
    clip: true; spacing: 4
    model: hits

    Timer {
        id: debounce; interval: 200
        onTriggered: {
            var q = results.query
            if (q.trim() === "") { results.hits = []; results.searched = false; return }
            results.app.call(results.app.backend.search(q), function (json) {
                if (q !== results.query) return   // typed on meanwhile
                try { results.hits = JSON.parse(json) } catch (e) { results.hits = [] }
                results.searched = true
            })
        }
    }
    onQueryChanged: debounce.restart()
    function refresh() { if (query.trim() !== "") debounce.restart() }

    delegate: Rectangle {
        width: ListView.view.width
        height: col.implicitHeight + 16
        radius: 4
        color: modelData.topicId === results.app.openId ? results.app.line : "transparent"
        Accessible.role: Accessible.Button
        Accessible.name: modelData.title
        Accessible.onPressAction: results.app.openThread(modelData.topicId)
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: results.app.openThread(modelData.topicId) }
        ColumnLayout {
            id: col
            anchors.left: parent.left; anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter; anchors.margins: 8
            spacing: 3
            Text { Layout.fillWidth: true; text: modelData.title; textFormat: Text.PlainText; font.bold: true
                   color: results.app.text; font.pixelSize: 14; elide: Text.ElideRight; maximumLineCount: 2; wrapMode: Text.WrapAtWordBoundaryOrAnywhere }
            Text { Layout.fillWidth: true; text: modelData.snippet.replace(/\s+/g, " "); textFormat: Text.PlainText
                   color: results.app.dim; font.pixelSize: 12; elide: Text.ElideRight; maximumLineCount: 2; wrapMode: Text.WrapAtWordBoundaryOrAnywhere }
            Text { Layout.fillWidth: true; textFormat: Text.PlainText; color: results.app.dim; font.pixelSize: 12; elide: Text.ElideRight
                   text: (modelData.kind === "reply" ? "reply · " : "topic · ") + results.app.listAuthor(modelData) + " · " + results.app.when(modelData.ts) }
        }
    }
    Text {
        anchors.centerIn: parent; width: parent.width - 32; horizontalAlignment: Text.AlignHCenter
        visible: results.searched && results.hits.length === 0
        wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: results.app.dim; font.pixelSize: 12
        text: "No post on this machine matches “" + results.query + "”."
    }
}
