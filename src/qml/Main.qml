import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Logos Forum. The view holds no forum state of its own: it asks the backend
// for the topic list and the open thread, and asks again whenever the backend
// says a post arrived. Every post shown has had its signature checked.
Item {
    id: root

    readonly property var backend: logos.module("logos_forum")
    property bool ready: false

    readonly property string status:        backend ? backend.status : ""
    readonly property string forumName:     backend ? backend.forumName : ""
    readonly property string appVersion:    backend ? backend.appVersion : ""
    readonly property string myLabel:       backend ? backend.myLabel : ""
    readonly property string myKey:         backend ? backend.myKey : ""
    readonly property string accountsJson:  backend ? backend.accountsJson : "[]"
    readonly property string rotationJson:  backend ? backend.rotationJson : "{}"
    readonly property int    outboxCount:   backend ? backend.outboxCount : 0
    readonly property string historyStatus: backend ? backend.historyStatus : ""

    property var topics: []
    property var accounts: []
    property string openId: ""
    property var openTopic: null
    property var openReplies: []
    property string lastError: ""
    // Ticks so that time-dependent labels ("sending…" → "waiting…") update.
    property double now: Date.now()
    Timer { interval: 5000; running: true; repeat: true; onTriggered: root.now = Date.now() }

    // Palette
    readonly property color bg: "#101114"
    readonly property color panel: "#17191e"
    readonly property color line: "#262a31"
    readonly property color text: "#e7e9ee"
    readonly property color dim: "#8b93a1"
    readonly property color accent: "#6ea8fe"
    readonly property color warn: "#f0b35b"

    function log(m) { console.log("[logos_forum qml] " + m) }

    function refreshTopics() {
        if (!ready) return
        logos.watch(backend.listTopics(), function (json) {
            try { root.topics = JSON.parse(json) } catch (e) { log("topics: " + e) }
        }, function (err) { log("listTopics failed: " + err) })
    }
    function refreshThread() {
        if (!ready || openId === "") return
        logos.watch(backend.thread(openId), function (json) {
            try {
                var t = JSON.parse(json)
                root.openTopic = t.topic
                root.openReplies = t.replies || []
            } catch (e) { log("thread: " + e) }
        }, function (err) { log("thread failed: " + err) })
    }
    function openThread(id) { openId = id; openTopic = null; openReplies = []; refreshThread() }
    function when(ms) {
        var d = new Date(ms), now = new Date()
        return d.toDateString() === now.toDateString()
            ? Qt.formatTime(d, "HH:mm") : Qt.formatDateTime(d, "d MMM HH:mm")
    }
    function result(r) {
        // Posting slots return the new id, or "error: …".
        if (r.indexOf("error: ") === 0) { lastError = r.substring(7); return false }
        lastError = ""
        return true
    }
    function plain(r) { lastError = r; return r === "" }
    // Slots on the replica answer asynchronously; `then` runs with the result.
    function call(pending, then) {
        logos.watch(pending, function (r) { if (then) then(r) },
                    function (err) { root.lastError = "The forum backend did not answer: " + err })
    }

    onAccountsJsonChanged: { try { accounts = JSON.parse(accountsJson) } catch (e) { accounts = [] } }
    onReadyChanged: { refreshTopics(); refreshThread() }

    Connections {
        target: logos
        function onViewModuleReadyChanged(name, isReady) {
            if (name === "logos_forum") root.ready = isReady && root.backend !== null
        }
    }
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onPostArrived(id, topicId) {
            root.refreshTopics()
            if (topicId === root.openId) root.refreshThread()
        }
        function onPostStateChanged(id, state, detail) {
            root.refreshTopics()
            root.refreshThread()
            if (state === "failed") root.lastError = "A post did not go out (" + detail + "); it will be retried."
        }
        function onOutboxCountChanged() { root.refreshTopics(); root.refreshThread() }
    }
    Component.onCompleted: {
        ready = backend !== null && logos.isViewModuleReady("logos_forum")
        try { accounts = JSON.parse(accountsJson) } catch (e) {}
    }

    Rectangle { anchors.fill: parent; color: root.bg }

    // ── Reusable pieces ───────────────────────────────────────────────────────

    component Label2: Text { color: root.text; font.pixelSize: 14; wrapMode: Text.Wrap }
    component Dim: Text { color: root.dim; font.pixelSize: 12; wrapMode: Text.Wrap }

    // Who a post is by, and how it was signed. The key is the one the signature
    // was checked against; the badge says whether it is an account, an alias
    // or a one-time anonymous key.
    component Byline: RowLayout {
        property var post
        spacing: 6
        Text {
            text: post ? post.author : ""
            color: post && post.mode === "anonymous" ? root.dim : root.accent
            font.pixelSize: 13; font.bold: true
        }
        Rectangle {
            visible: post && post.mode !== "identity"
            radius: 3; color: root.line
            implicitWidth: badge.implicitWidth + 8; implicitHeight: badge.implicitHeight + 2
            Text { id: badge; anchors.centerIn: parent; color: root.dim; font.pixelSize: 10
                   text: post && post.mode === "alias" ? "alias" : "anonymous" }
        }
        Text {
            text: post ? "✓ " + post.authorKey.substring(0, 8) : ""
            color: root.dim; font.pixelSize: 11; font.family: "monospace"
            ToolTip.visible: keyHover.hovered
            ToolTip.text: post ? "Signature verified against key " + post.authorKey : ""
            HoverHandler { id: keyHover }
        }
        Dim { text: post ? "· " + root.when(post.ts) : "" }
        Text {
            visible: post && post.state !== ""
            // A post the node accepted but the network has not confirmed stays
            // "sending" for a few seconds; after that, say what is going on.
            text: post ? ((post.state === "sending" && root.now - post.ts < 30000)
                          ? "· sending…" : "· waiting for the network — will retry") : ""
            color: root.warn; font.pixelSize: 12
        }
    }

    // Choose how to sign the next post.
    component SignAs: RowLayout {
        property alias mode: modeBox.currentIndex
        property alias alias: aliasField.text
        spacing: 8
        Dim { text: "Post as" }
        ComboBox {
            id: modeBox
            model: [root.myLabel || "account", "alias", "anonymous"]
            implicitWidth: 150
        }
        TextField {
            id: aliasField
            visible: modeBox.currentIndex === 1
            placeholderText: "alias"
            maximumLength: 40
            implicitWidth: 140
        }
        Dim {
            Layout.fillWidth: true
            text: modeBox.currentIndex === 2 ? "A key made for this post only, then wiped."
                : modeBox.currentIndex === 1 ? "Signed by " + root.myLabel + ", shown under the alias."
                : "Signed by " + root.myLabel + " (" + root.myKey + ")."
        }
    }

    // ── Layout ────────────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // Header: forum, node, accounts
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Text { text: root.forumName; color: root.text; font.pixelSize: 20; font.bold: true }
            Dim { text: "v" + root.appVersion }
            Item { Layout.fillWidth: true }
            Dim { text: (root.status === "Connected" ? "● " : "○ ") + root.status }
            Dim { visible: root.outboxCount > 0; color: root.warn
                  text: root.outboxCount + " waiting to send" }
            ComboBox {
                id: accountBox
                implicitWidth: 170
                model: root.accounts.map(function (a) { return a.label + "  " + a.key })
                currentIndex: root.accounts.findIndex(function (a) { return a.selected })
                onActivated: function (i) { root.call(root.backend.selectAccount(root.accounts[i].label), root.plain) }
            }
            Button { text: "Accounts…"; onClicked: accountsDialog.open() }
        }
        Dim { Layout.fillWidth: true; text: root.historyStatus }
        Text {
            Layout.fillWidth: true; visible: root.lastError !== ""
            text: root.lastError; color: root.warn; font.pixelSize: 13; wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // Topics
            Rectangle {
                Layout.preferredWidth: Math.max(280, root.width * 0.34)
                Layout.fillHeight: true
                color: root.panel; radius: 6; border.color: root.line

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "Topics"; color: root.text; font.pixelSize: 15; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button { text: "↻"; onClicked: root.call(root.backend.catchUp(), function () { root.refreshTopics() })
                                 ToolTip.visible: hovered; ToolTip.text: "Fetch what was posted while you were away" }
                        Button { text: "New topic"; highlighted: true; onClicked: newTopic.open() }
                    }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true; spacing: 4
                        model: root.topics
                        delegate: Rectangle {
                            width: ListView.view.width
                            height: col.implicitHeight + 16
                            radius: 4
                            color: modelData.id === root.openId ? root.line : "transparent"
                            MouseArea { anchors.fill: parent; onClicked: root.openThread(modelData.id) }
                            ColumnLayout {
                                id: col
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter; anchors.margins: 8
                                spacing: 2
                                Label2 { Layout.fillWidth: true; text: modelData.title; font.bold: true; maximumLineCount: 2; elide: Text.ElideRight }
                                Dim { Layout.fillWidth: true
                                      text: modelData.author + " · " + modelData.replies + (modelData.replies === 1 ? " reply" : " replies")
                                            + " · " + root.when(modelData.last)
                                            + (modelData.state !== "" ? " · not sent yet" : "") }
                            }
                        }
                        Dim { anchors.centerIn: parent; visible: root.topics.length === 0
                              text: "No topics yet. Start one, or wait for peers." }
                    }
                }
            }

            // Thread
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: root.panel; radius: 6; border.color: root.line

                Dim { anchors.centerIn: parent; visible: root.openId === ""; text: "Pick a topic." }

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 14; spacing: 10
                    visible: root.openId !== ""

                    ScrollView {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true
                        ColumnLayout {
                            width: parent.width
                            spacing: 14
                            Text {
                                Layout.fillWidth: true
                                text: root.openTopic ? root.openTopic.title : "Topic not received yet — its replies are shown below."
                                color: root.text; font.pixelSize: 18; font.bold: true; wrapMode: Text.Wrap
                            }
                            Byline { post: root.openTopic; visible: root.openTopic !== null }
                            Label2 { Layout.fillWidth: true; text: root.openTopic ? root.openTopic.body : ""; textFormat: Text.PlainText }
                            Rectangle { Layout.fillWidth: true; height: 1; color: root.line }
                            Repeater {
                                model: root.openReplies
                                delegate: ColumnLayout {
                                    Layout.fillWidth: true; spacing: 4
                                    Byline { post: modelData }
                                    Label2 { Layout.fillWidth: true; text: modelData.body; textFormat: Text.PlainText }
                                }
                            }
                        }
                    }

                    // Reply
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        TextArea {
                            id: replyBody
                            Layout.fillWidth: true; Layout.preferredHeight: 80
                            placeholderText: "Write a reply"; wrapMode: TextArea.Wrap
                            color: root.text; placeholderTextColor: root.dim
                            background: Rectangle { color: root.bg; radius: 4; border.color: root.line }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            SignAs { id: replyAs; Layout.fillWidth: true }
                            Button {
                                text: "Reply"; highlighted: true
                                enabled: replyBody.text.trim().length > 0
                                onClicked: root.call(root.backend.reply(root.openId, replyBody.text, replyAs.mode, replyAs.alias), function (r) {
                                    if (root.result(r)) {
                                        replyBody.text = ""
                                        root.refreshThread(); root.refreshTopics()
                                    }
                                })
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Dialogs ───────────────────────────────────────────────────────────────

    Dialog {
        id: newTopic
        background: Rectangle { color: root.panel; radius: 8; border.color: root.line }
        header: Text { text: newTopic.title; color: root.text; font.pixelSize: 16; font.bold: true; padding: 16 }
        title: "New topic"
        modal: true; anchors.centerIn: parent
        width: Math.min(640, root.width - 40)
        footer: Item {
            implicitHeight: 56
            Button { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter
                     text: "Cancel"; onClicked: newTopic.close() }
        }
        ColumnLayout {
            anchors.fill: parent; spacing: 8
            TextField { id: topicTitle; Layout.fillWidth: true; placeholderText: "Title"; maximumLength: 200 }
            TextArea { id: topicBody; Layout.fillWidth: true; Layout.preferredHeight: 160; placeholderText: "What do you want to say?"; wrapMode: TextArea.Wrap
                            color: root.text; placeholderTextColor: root.dim
                            background: Rectangle { color: root.bg; radius: 4; border.color: root.line } }
            SignAs { id: topicAs; Layout.fillWidth: true }
            Button {
                Layout.alignment: Qt.AlignRight
                text: "Post"; highlighted: true
                enabled: topicTitle.text.trim().length > 0 && topicBody.text.trim().length > 0
                onClicked: root.call(root.backend.createTopic(topicTitle.text, topicBody.text, topicAs.mode, topicAs.alias), function (r) {
                    if (root.result(r)) {
                        topicTitle.text = ""; topicBody.text = ""
                        newTopic.close()
                        root.refreshTopics(); root.openThread(r)
                    }
                })
            }
        }
    }

    Dialog {
        id: accountsDialog
        background: Rectangle { color: root.panel; radius: 8; border.color: root.line }
        header: Text { text: accountsDialog.title; color: root.text; font.pixelSize: 16; font.bold: true; padding: 16 }
        title: "Accounts"
        modal: true; anchors.centerIn: parent
        width: Math.min(820, root.width - 40)
        footer: Item {
            implicitHeight: 56
            Button { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter
                     text: "Close"; onClicked: accountsDialog.close() }
        }
        property var rot: { try { return JSON.parse(root.rotationJson) } catch (e) { return {} } }
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            Repeater {
                model: root.accounts
                delegate: RowLayout {
                    Layout.fillWidth: true
                    Layout.maximumWidth: accountsDialog.availableWidth
                    Label2 { text: (modelData.selected ? "● " : "○ ") + modelData.label }
                    Dim { text: modelData.key + " · " + modelData.posts + " posts on this key"; Layout.fillWidth: true }
                    Button { text: "Use"; enabled: !modelData.selected; onClicked: root.call(root.backend.selectAccount(modelData.label), root.plain) }
                    Button { text: "Delete"; enabled: root.accounts.length > 1; onClicked: root.call(root.backend.deleteAccount(modelData.label), root.plain) }
                }
            }
            RowLayout {
                TextField { id: newLabel; placeholderText: "new account name"; Layout.fillWidth: true }
                Button { text: "Create"; onClicked: root.call(root.backend.createAccount(newLabel.text), function (r) { if (root.plain(r)) newLabel.text = "" }) }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.line }
            Label2 { text: "Identity rotation"; font.bold: true }
            Dim { Layout.fillWidth: true
                  text: "Replace the selected account's key with a fresh one. Nothing links the old key to the new one, so earlier posts stop being attributable to what you post next." }
            RowLayout {
                Button { text: "Rotate now"; onClicked: root.call(root.backend.rotateAccount(), root.plain) }
                Dim { text: "Automatically after" }
                SpinBox { id: rotPosts; implicitWidth: 120; from: 0; to: 10000; value: accountsDialog.rot.maxPosts || 0; editable: true }
                Dim { text: "posts or" }
                SpinBox { id: rotDays; implicitWidth: 120; from: 0; to: 3650; value: accountsDialog.rot.maxDays || 0; editable: true }
                Dim { text: "days" }
                Button { text: "Save"; onClicked: root.call(root.backend.setRotation(rotPosts.value, rotDays.value), root.plain) }
            }
            Dim { text: "0 means never." }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.line }
            Label2 { text: "History"; font.bold: true }
            RowLayout {
                Button { text: "Fetch missed posts"; onClicked: root.call(root.backend.catchUp()) }
                Button { text: "Save snapshot to Logos Storage"; onClicked: root.call(root.backend.saveSnapshot(), root.plain) }
            }
        }
    }
}
