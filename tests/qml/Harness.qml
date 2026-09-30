import QtQuick
import QtQuick.Controls

// Renders the real Main.qml against a stand-in backend, at a given size and in
// a given state, and saves a screenshot. Used to check the layout at every
// width from a phone to a wide desktop without running Basecamp.
//
//   qml tests/qml/Harness.qml -- <width> <height> <state> <out.png>
//   out.png "live" keeps the window open (to resize it by hand) instead of saving.
//   state: welcome | list | thread | alias | anonymous | newtopic | accounts | offline | stress | inject
//   (inject: a title and an alias carrying <img> tags pointing at 127.0.0.1:8977, which must not be fetched)
Window {
    id: win
    readonly property var args: Qt.application.arguments.slice(Qt.application.arguments.indexOf("--") + 1)
    width: parseInt(args[0] || "1200"); height: parseInt(args[1] || "800")
    readonly property string state: args[2] || "thread"
    visible: true
    color: "#101114"

    // What Basecamp gives a module view: `logos.module(name)` and `logos.watch`.
    QtObject {
        id: logos
        signal viewModuleReadyChanged(string name, bool isReady)
        function module(name) { return backend }
        function isViewModuleReady(name) { return true }
        function watch(pending, ok, err) { ok(pending) }
    }

    QtObject {
        id: backend
        signal postArrived(string id, string topicId)
        signal postStateChanged(string id, string state, string detail)
        property string status: win.state === "offline" ? "Joined, waiting for peers"
                              : win.state === "inject" ? 'Node failed <img src="http://127.0.0.1:8977/node.png">' : "Connected"
        property string forumName: "Logos Forum"
        property string appVersion: "0.1.9"
        property string myLabel: "Alice"
        property string myKey: "8b0adc2e41f07a9d3c5e6b1a2f4d8c9e0b7a6f5e4d3c2b1a0f9e8d7c6b5a6747"
        property string accountsJson: JSON.stringify([
            { label: "Alice", key: myKey, selected: true, posts: 12 },
            { label: "Work", key: "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", selected: false, posts: 3 }])
        property string rotationJson: JSON.stringify({ maxPosts: 0, maxDays: 30 })
        property int outboxCount: win.state === "offline" ? 1 : 0
        property string historyStatus: win.state === "offline" ? "History: no store node reachable, asking peers · Storage ready"
                                     : win.state === "inject" ? 'History: store node said 400 <img src="http://127.0.0.1:8977/status.png">'
                                                              : "Caught up 14:02 · 0 new · Storage ready"
        readonly property double t0: Date.now()
        function post(id, author, key, mode, mine, ago, body, extra) {
            var p = { id: id, author: author, authorKey: key, mode: mode, mine: mine, ts: t0 - ago * 60000, body: body, state: "" }
            for (var k in extra) p[k] = extra[k]
            return p
        }
        readonly property var topicList: [
            post("t1", "32e5…f39c", "32e5a1c09d8b7f6e5d4c3b2a1f0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4af39c", "identity", false, 24,
                 "Heard end of September. The team says they are testing and reviewing everything right now. Does anyone know if LEZ state gets reset again?",
                 { title: "When does testnet v0.3 land?", replies: 4, last: t0 - 60000, unread: true }),
            post("t2", "Alice", myKey, "identity", true, 180,
                 "Dogfooding the RC this week. Package repositories over HTTPS work, the storage module needed the new API.",
                 { title: "Basecamp 0.3.0 RC: what broke for you?", replies: 7, last: t0 - 3600000, unread: false }),
            post("t3", "anonymous", "6b79c6d4e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5", "anonymous", false, 1500,
                 "The update to 1.1.1 says install failed, then reports a conflict. Removing the old one first fixed it for me.",
                 { title: "LEZ wallet 1.1.1 update fails to install", replies: 2, last: t0 - 90000000, unread: false }),
            post("t4", "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 3000,
                 "Bi-weekly session on Friday. Bring code, architecture questions, or just come and see what others build.",
                 { title: "Builders Meetup this Friday: what are you demoing?", replies: 5, last: t0 - 170000000, unread: false })
        ]
        readonly property var stressTopic: post("t1", "A-forty-character-alias-that-never-ends-x", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 5,
            "Logs at https://github.com/logos-co/logos-basecamp/releases/tag/0.3.0-rc.1/assets/LogosBasecamp-0.3.0-rc.1-aarch64-linux.AppImage and key 6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a",
            { title: "A very long topic title about the next testnet, the reset, the migration of every deployed program, and what builders should do on day one", replies: 1, last: t0, unread: true, state: "sending" })
        readonly property var injectTopic: post("t1", '<img src="http://127.0.0.1:8977/alias.png">', "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 5,
            '<img src="http://127.0.0.1:8977/body.png"> body', { title: '<img src="http://127.0.0.1:8977/title.png"><b>✓ verified</b>', replies: 0, last: t0, unread: true })
        function listTopics() { return JSON.stringify(win.state === "stress" ? [stressTopic].concat(topicList.slice(1))
                                                      : win.state === "inject" ? [injectTopic] : topicList) }
        function thread(id) {
            var replies = [
                post("r1", "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 23,
                     "They said it could land before the end of the month. Keep your deploy scripts ready."),
                post("r2", "anonymous", "6b79c6d4e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5", "anonymous", false, 22,
                     "Expect a reset. Deployments on 0.2.4 will most likely not carry over."),
                post("r3", "Alice", myKey, "identity", true, 2, "Rotated my key before posting this. Anyone else redeploying on day one?"),
                post("r4", "Bob", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", true, 0,
                     "Written on a train with no signal. It goes out when I'm back online.", { state: win.state === "offline" ? "sending" : "" })
            ]
            if (win.state === "stress") return JSON.stringify({ topic: stressTopic, replies: [stressTopic] })
            if (win.state === "inject") return JSON.stringify({ topic: injectTopic, replies: [injectTopic] })
            return JSON.stringify({ topic: topicList[0], replies: win.state === "offline" ? replies : replies.slice(0, 3) })
        }
        function markRead(id) { return "" }
        function catchUp() { return "" }
        function reply() { return "r9" }
        function createTopic() { return "t9" }
        function selectAccount() { return "" }
        function createAccount() { return "" }
        function deleteAccount() { return "" }
        function rotateAccount() { return "" }
        function setRotation() { return "" }
        function saveSnapshot() { return "" }
    }

    Loader { id: view; anchors.fill: parent; source: "../../src/qml/Main.qml" }

    // Positive control for scripts/check-no-remote-fetch.py: this one element is
    // rich text on purpose, so a check that sees no request at all has proven
    // nothing and fails.
    Text { visible: win.state === "inject"; textFormat: Text.RichText; text: '<img src="http://127.0.0.1:8977/control.png">' }

    function find(item, pred) {
        if (!item) return null
        if (pred(item)) return item
        var kids = (item.children || []).concat(item.data || [])
        for (var i = 0; i < kids.length; i++) {
            if (kids[i] === item) continue
            var r = find(kids[i], pred); if (r) return r
        }
        return null
    }
    Timer {
        interval: 400; running: view.status === Loader.Ready
        onTriggered: {
            var m = view.item
            if (win.state !== "welcome" && win.state !== "list" && win.state !== "newtopic") m.openThread("t1")
            if (win.state === "list") m.openId = ""
            var replyAs = find(m, function (o) { return o.objectName === "replyAs" })
            if (win.state === "alias") { replyAs.mode = 1; replyAs.alias = "Ghost" }
            if (win.state === "stress") { replyAs.mode = 1; replyAs.alias = "A-forty-character-alias-that-never-ends-x" }
            if (win.state === "inject") m.lastError = 'A post did not go out (<img src="http://127.0.0.1:8977/error.png">); it will be retried.'
            if (win.state === "anonymous") replyAs.mode = 2
            var popups = m.data.filter(function (o) { return o.title !== undefined && o.open !== undefined })
            if (win.state === "newtopic") popups.filter(function (p) { return p.title === "New topic" })[0].open()
            if (win.state === "accounts") popups.filter(function (p) { return p.title === "Accounts" })[0].open()
            if (win.args[3] !== "live") shot.start()
        }
    }
    Timer {
        id: shot; interval: 900
        onTriggered: win.contentItem.grabToImage(function (r) { r.saveToFile(win.args[3] || "/tmp/shot.png"); Qt.quit() })
    }
}
