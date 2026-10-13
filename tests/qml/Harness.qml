import QtQuick
import QtQuick.Controls
import "sample-images.js" as Pics
import "../../src/qml/Markdown.js" as Md

// Renders the real Main.qml against a stand-in backend, at a given size and in
// a given state, and saves a screenshot. Used to check the layout at every
// width from a phone to a wide desktop without running Basecamp.
//
//   qml tests/qml/Harness.qml -- <width> <height> <state> <out.png>
//   out.png "live" keeps the window open (to resize it by hand) instead of saving.
//   state: welcome | list | thread | alias | anonymous | newtopic | accounts | offline | stress | inject | links
//          | images | imagefail | imagecompose | search
//   (images: posts with images and reactions; imagefail: a download that does not match its signed hash;
//    imagecompose: the composer with two attachments; search: results for "reset")
//          | private | create | join | invite | leave | muted   (private forums and moderation)
//   (inject: a title, an alias and bodies carrying <img> tags, https links and plain http URLs pointing at 127.0.0.1:8977, none to be fetched)
//   (links: a thread whose posts carry links, for screenshots)
//
//   qml tests/qml/Harness.qml -- 800 600 linkify   unit cases for linkify/safeUrl, exit 1 on any failure
//   qml tests/qml/Harness.qml -- 800 600 authors   who the topic list names, exit 1 on any failure
//   qml tests/qml/Harness.qml -- 800 600 scroll    auto-scroll behaviour of the thread, exit 1 on any failure
//   qml tests/qml/Harness.qml -- 800 600 media     images and reactions: only backend data: URLs, nothing loaded without a click
//   qml tests/qml/Harness.qml -- 800 600 markdown  adversarial cases for Markdown.js, exit 1 on any failure
//   states locked | inbox | backup: keys sealed and locked; the inbox; an account's recovery phrase
//   state markdown-shot: a thread whose posts use every Markdown feature, for screenshots
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
            { label: "Alice", key: myKey, selected: true, posts: 12, backedUp: true, handle: myKey.substring(0, 16) },
            { label: "Work", key: "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", selected: false, posts: 3, backedUp: false, handle: "43ca10be77f0e2d9" }])
        property string keysJson: JSON.stringify({ protected: win.state !== "list" && win.state !== "backup", unlocked: win.state !== "locked" })
        property int inboxUnread: win.state === "list" ? 0 : 2
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
            '<img src="http://127.0.0.1:8977/body.png"> body, see https://127.0.0.1:8977/link.html. Plain http is no link: http://127.0.0.1:8977/plain-http',
            { title: '<img src="http://127.0.0.1:8977/title.png"><b>✓ verified</b> https://127.0.0.1:8977/title-link', replies: 0, last: t0, unread: true })
        // Links that try to smuggle a tag or an attribute in, each one pointing at the check's server.
        readonly property var injectReplies: [
            post("r1", "Mallory", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 3,
                 '<img src=https://127.0.0.1:8977/x.png>https://127.0.0.1:8977/after-img <img src="//127.0.0.1:8977/schemeless.png" width="9" height="9">'),
            post("r2", "Mallory", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 2,
                 'https://127.0.0.1:8977/q"><img src="http://127.0.0.1:8977/quote.png">\nhttps://127.0.0.1:8977/a\'onmouseover=x <a href="https://127.0.0.1:8977/anchor">a</a>'),
            post("r3", "Mallory", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 1,
                 'javascript:alert(1) file:///etc/passwd data:text/html,<img src=http://127.0.0.1:8977/data.png> https://127.0.0.1:8977/tls http://127.0.0.1:8977/http'),
            // Markdown, written to load something: image syntax, a backslash
            // before a tag, a tag inside emphasis, code and a quote, and links
            // whose target tries to leave the href.
            post("r4", "Mallory", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 0,
                 '![x](https://127.0.0.1:8977/md-image.png) \\<img src="http://127.0.0.1:8977/md-backslash.png"> \\\\<img src=http://127.0.0.1:8977/md-double.png>\n'
                 + '**<img src=http://127.0.0.1:8977/md-bold.png>** _<img src=http://127.0.0.1:8977/md-italic.png>_ `<img src=http://127.0.0.1:8977/md-code.png>`\n'
                 + '> <img src=http://127.0.0.1:8977/md-quote.png>\n- <img src=http://127.0.0.1:8977/md-list.png>\n'
                 + '```\n<img src=http://127.0.0.1:8977/md-fence.png>\n```\n'
                 + '[a](https://127.0.0.1:8977/md"><img src=http://127.0.0.1:8977/md-attr.png>) [b](javascript:alert(1)) [c](http://127.0.0.1:8977/md-http) '
                 + '&#60;img src=http://127.0.0.1:8977/md-entity.png&#62; [d](https://127.0.0.1:8977/md-link)')
        ]
        // Scroll state: a long thread that grows on demand.
        property int extra: 0
        readonly property var linkTopic: post("t1", "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 30,
            "The release notes are at https://github.com/logos-co/logos-basecamp/releases and the testnet status at https://status.logos.co.\nWhat broke for you?",
            { title: "Basecamp 0.3.0: links and notes", replies: 2, last: t0, unread: false })
        readonly property var mdTopic: post("t1", "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 30,
            "Storage fails to start after the **0.3.0** update, with *this* in the log:\n\n```\nFailed to load Mix relay pool:\n  Invalid mixPubKey in pool entry\n```\n\n- macOS 15, Apple silicon\n- `storage_module` 3.0.0\n  - fresh install\n\nAnyone seen it? ___Really___ stuck.",
            { title: "Storage fails after the update", replies: 2, last: t0, unread: false })
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
            if (win.state === "images" || win.state === "imagefail" || win.state === "media")
                return JSON.stringify({ topic: imageTopic, replies: imageReplies() })
            if (win.state === "stress") return JSON.stringify({ topic: stressTopic, replies: [stressTopic] })
            if (win.state === "inject") {
                // Images whose thumbnails and full image are not the backend's
                // own data: URLs (a URL to the check's server, an SVG that
                // would fetch one): none may be loaded.
                var withImages = Object.assign({}, injectTopic, { images: [
                    img(0, true, { thumb: "http://127.0.0.1:8977/thumb.png" }),
                    img(0, true, { thumb: "data:image/svg+xml;base64," + Qt.btoa('<svg xmlns="http://www.w3.org/2000/svg"><image href="http://127.0.0.1:8977/svg.png"/></svg>') }),
                    img(0, true, { thumb: "https://127.0.0.1:8977/thumb-https.png" }) ], imagesMissing: 0 })
                return JSON.stringify({ topic: withImages, replies: [injectTopic].concat(injectReplies) })
            }
            if (win.state === "markdown-shot") return JSON.stringify({ topic: mdTopic, replies: [
                post("r1", "Alice", myKey, "identity", true, 20, "Same here. What fixed it for me:\n\n1. stop Basecamp\n2. run `rm -rf ~/basecamp/module_data/storage`\n3. start it again\n\n> Expect a reset. Deployments on 0.2.4 will most likely not carry over.\n\nSo **back up first**, see [the notes](https://github.com/logos-co/logos-basecamp/releases)."),
                post("r2", "Bob", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 1, "Shown, not run: <img src=x> and \\*not italic\\*, snake_case_names stay as typed.")] })
            if (win.state === "links") return JSON.stringify({ topic: linkTopic, replies: [
                post("r1", "Alice", myKey, "identity", true, 20, "Mine is fine. Changelog (with the storage API change): https://github.com/logos-co/logos-storage-module/blob/master/CHANGELOG.md, worth a read."),
                post("r2", "Bob", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 1, "Not links: javascript:alert(1) and plain http://example.org. A link: https://example.org/a?b=1&c=2 (in brackets).")] })
            if (win.state === "scroll") {
                var many = []
                for (var i = 0; i < 25 + extra; i++)
                    many.push(post("s" + i, "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 60 - i,
                                   "Reply number " + i + ". Long enough to take a line or two of the pane, so that twenty-five of them overflow it."))
                return JSON.stringify({ topic: topicList[0], replies: many })
            }
            return JSON.stringify({ topic: topicList[0], replies: win.state === "offline" ? replies : replies.slice(0, 3) })
        }
        // Private forums and moderation.
        readonly property bool inGroup: ["private", "invite", "leave"].indexOf(win.state) >= 0
        property string currentForum: inGroup ? "g:5f87b6d87b776f47eebb30ac7baaec95" : ""
        property string forumsJson: JSON.stringify([
            { id: "", name: "Logos Forum", private: false, current: currentForum === "" },
            { id: "g:5f87b6d87b776f47eebb30ac7baaec95", name: "Core contributors", private: true, current: currentForum !== "" },
            { id: "g:0a1b2c3d4e5f60718293a4b5c6d7e8f9", name: "Night shift", private: true, current: false }])
        function selectForum(id) { currentForum = id; return "" }
        function createGroup(name) { return "logosforum://join/AQdHzx3nW1Jm4yU0mY2c1Q9f8x7v6u5t4s3r2q1p0o9n8m7l6k5j4i3Q29yZSBjb250cmlidXRvcnM" }
        function groupInvite(id) { return createGroup("") }
        function joinGroup(invite) { return invite.indexOf("logosforum://join/") === 0 ? "" : "error: this is not a Logos Forum invite (logosforum://join/…)" }
        function leaveGroup(id) { currentForum = ""; return "" }
        function muteAuthor(key) { return "" }
        function unmuteAuthor(key) { return "" }
        function hidePost(id) { return "" }
        function unhidePost(id) { return "" }
        function moderationList() { return JSON.stringify({
            muted: [ { key: "32e5a1c09d8b7f6e5d4c3b2a1f0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4af39c", at: t0 - 3600000 },
                     { key: "6b79c6d4e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5", at: t0 - 86400000 * 3 } ],
            hidden: [ { id: "t3", title: "LEZ wallet 1.1.1 update fails to install", at: t0 - 600000 } ] }) }
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
        function unlock(p) { return p === "right" ? "" : "wrong passphrase" }
        function lockKeys() { return "" }
        function setPassphrase(c, n) { return "" }
        function recoveryPhrase(label, pass) {
            return "legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth title"
        }
        function restoreAccount(label, words) { return "" }
        function markInboxRead() { return "" }
        function inbox() {
            return JSON.stringify({ handle: myKey.substring(0, 16), unread: 2, items: [
                post("r7", "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 3,
                     "Tried it on Linux too: same error, same fix. Thanks for writing it up.",
                     { why: "topic", unread: true, topicId: "t2", topicTitle: "Basecamp 0.3.0 RC: what broke for you?" }),
                post("r8", "43ca…53a0", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 30,
                     "@" + myKey.substring(0, 16) + " you ran this last week, did the reset keep your memberships?",
                     { why: "mention", unread: true, topicId: "t1", topicTitle: "When does testnet v0.3 land?" }),
                post("r9", "anonymous", "6b79c6d4e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5", "anonymous", false, 1500,
                     "Same for me after your reply: removing the old package first fixed it.",
                     { why: "after", unread: false, topicId: "t3", topicTitle: "LEZ wallet 1.1.1 update fails to install" })] })
        }
        function myPosts() {
            return JSON.stringify([
                post("t2", "Alice", myKey, "identity", true, 180, "Dogfooding the RC this week.", { title: "Basecamp 0.3.0 RC: what broke for you?", topicId: "t2", topicTitle: "Basecamp 0.3.0 RC: what broke for you?" }),
                post("r3", "Alice", myKey, "identity", true, 2, "Rotated my key before posting this.", { topicId: "t1", topicTitle: "When does testnet v0.3 land?" })])
        }
        // What the view asked to open, in order (a click calls this).
        property var opened: []
        function openLink(url) { opened.push(url); console.log("click opens " + url); return "" }

        // ── Images, reactions, search ────────────────────────────────────────
        signal imageUploaded(string token, string state, string detail)
        signal imageState(string postId, int index, string state, string detail)
        signal reactionsChanged(string postId)
        property var loads: []          // what the view asked to load, in order
        property int attached: 0
        function img(n, cached, extra) {
            var o = { w: 1200, h: 800, bytes: n === 0 ? Pics.bytes0 : Pics.bytes1, type: "image/jpeg",
                      thumb: n === 0 ? Pics.thumb0 : Pics.thumb1, cached: cached }
            for (var k in extra) o[k] = extra[k]
            return o
        }
        function attachImage(url) {
            var t = "tok" + (attached++)
            var thumb = attached % 2 ? Pics.thumb0 : Pics.thumb1
            if (win.state !== "imagecompose" || attached === 1) Qt.callLater(function () { backend.imageUploaded(t, "ready", "") })
            return JSON.stringify({ token: t, thumb: thumb, w: 1200, h: 800, bytes: 180000, type: "image/jpeg" })
        }
        function discardImage(t) { return "" }
        function createTopicWithImages() { return "t9" }
        function replyWithImages() { return "r9" }
        function loadImage(id, i) {
            loads.push(id + "/" + i)
            console.log("load image " + id + "/" + i)
            var fail = win.state === "imagefail" && id === "r1"
            imageState(id, i, "loading", "")
            var t = Qt.createQmlObject('import QtQuick; Timer { interval: 300 }', win)
            t.triggered.connect(function () {
                backend.imageState(id, i, fail ? "failed" : "ready", fail ? "the bytes received are not the image the author signed: not shown" : "")
                t.destroy()
            })
            t.start()
            return ""
        }
        function fullImage(id, i) {
            if (win.state === "inject") return "http://127.0.0.1:8977/full.png"
            return (id === "t1" && i === 1) || id === "r2" ? Pics.full1 : Pics.full0
        }
        property var reacted: ({})
        function react(id, e) { var r = Object.assign({}, reacted); r[id + e] = !r[id + e]; reacted = r; reactionsChanged(id); return "" }
        function counts(id, base) {
            var c = base.slice(), mine = [false, false, false, false, false]
            for (var e = 0; e < 5; e++) if (reacted[id + e]) { c[e]++; mine[e] = true }
            return { reactions: c, reacted: mine }
        }
        function search(q) {
            var all = topicList.map(function (t) { return { id: t.id, topicId: t.id, kind: "topic", title: t.title, snippet: t.body,
                                                             mode: t.mode, author: t.author, authorKey: t.authorKey, mine: t.mine, ts: t.ts } })
            all.push({ id: "r2", topicId: "t1", kind: "reply", title: topicList[0].title, snippet: "Expect a reset. Deployments on 0.2.4 will most likely not carry over.",
                       mode: "anonymous", author: "", authorKey: "6b79c6d4e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5", mine: false, ts: t0 - 22 * 60000 })
            var w = q.toLowerCase().split(/\s+/).filter(function (x) { return x !== "" })
            return JSON.stringify(all.filter(function (h) {
                return w.every(function (x) { return (h.title + " " + h.snippet).toLowerCase().indexOf(x) >= 0 })
            }))
        }
        readonly property var imageTopic: post("t1", "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 40,
            "From the ridge last night, after the meetup. Metadata stripped on the way out, as promised.",
            Object.assign({ title: "Sunset over the ridge", replies: 3, last: t0, unread: false, imageCount: 2,
                            images: [img(0, true), img(1, false)], imagesMissing: 0 }, counts("t1", [3, 1, 2, 0, 0])))
        function imageReplies() {
            return [
                post("r1", "Alice", myKey, "identity", true, 20, "Same evening, other side of the valley:",
                     Object.assign({ imageCount: 1, images: [img(1, false)], imagesMissing: 0 }, counts("r1", [1, 0, 0, 1, 0]))),
                post("r2", "anonymous", "6b79c6d4e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5", "anonymous", false, 10,
                     "This copy reached me through an older node.", Object.assign({ imageCount: 1, images: [], imagesMissing: 1 }, counts("r2", [0, 0, 0, 0, 0]))),
                post("r3", "Bob", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 2,
                     "Beautiful. Are these straight from the phone?", counts("r3", [2, 0, 0, 0, 1]))
            ]
        }
    }

    Loader { id: view; anchors.fill: parent; source: "../../src/qml/Main.qml" }

    // Positive control for scripts/check-no-remote-fetch.py: this one element is
    // rich text on purpose, so a check that sees no request at all has proven
    // nothing and fails.
    Text { visible: win.state === "inject"; textFormat: Text.RichText; text: '<img src="http://127.0.0.1:8977/control.png">' }
    // And StyledText, which the links use, would fetch an <img> too: only the
    // escaping keeps one from existing.
    Text { visible: win.state === "inject"; textFormat: Text.StyledText; text: '<img src="http://127.0.0.1:8977/control-styled.png" width="1" height="1">' }

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
    // ── linkify: what a post's text may turn into ─────────────────────────────
    function linkifyCases(m) {
        var fails = 0
        function eq(name, got, want) {
            if (got !== want) { fails++; console.log("FAIL " + name + "\n  got:  " + got + "\n  want: " + want) }
            else console.log("ok   " + name)
        }
        // Whatever the input, the output holds no tag but <a href="…"> … </a> and <br>,
        // and no raw quote outside an href's own delimiters.
        function onlyLinks(name, input) {
            var out = m.linkify(input)
            var stripped = out.replace(/<a href="[^"<>']*">[^"<>']*<\/a>/g, "").replace(/<br>/g, "")
            if (/[<>"']/.test(stripped)) { fails++; console.log("FAIL " + name + ": stray markup in " + out) }
            else console.log("ok   " + name + " (only <a>/<br>)")
            return out
        }
        eq("plain text is escaped", m.linkify("a < b & c > \"d\" 'e'"), "a &lt; b &amp; c &gt; &quot;d&quot; &#39;e&#39;")
        eq("an https link", m.linkify("see https://a.org/x"), 'see <a href="https://a.org/x">https://a.org/x</a>')
        eq("plain http is not a link", m.linkify("see http://a.org/x"), "see http://a.org/x")
        eq("upper-case HTTP is not a link", m.linkify("HTTP://A.org/"), "HTTP://A.org/")
        eq("upper-case scheme", m.linkify("HTTPS://A.org/"), '<a href="HTTPS://A.org/">HTTPS://A.org/</a>')
        eq("trailing full stop", m.linkify("at https://a.org/x."), 'at <a href="https://a.org/x">https://a.org/x</a>.')
        eq("trailing comma", m.linkify("https://a.org, then"), '<a href="https://a.org">https://a.org</a>, then')
        eq("in brackets", m.linkify("(https://a.org/x)"), '(<a href="https://a.org/x">https://a.org/x</a>)')
        eq("in square brackets", m.linkify("[https://a.org/x]"), '[<a href="https://a.org/x">https://a.org/x</a>]')
        eq("trailing ! and ?", m.linkify("https://a.org/x!? ok"), '<a href="https://a.org/x">https://a.org/x</a>!? ok')
        eq("query with &", m.linkify("https://a.org/?x=1&y=2"), '<a href="https://a.org/?x=1&amp;y=2">https://a.org/?x=1&amp;y=2</a>')
        eq("a URL ending in & keeps it", m.linkify("https://a.org/?x&"), '<a href="https://a.org/?x&amp;">https://a.org/?x&amp;</a>')
        eq("stops at whitespace", m.linkify("https://a.org/x y"), '<a href="https://a.org/x">https://a.org/x</a> y')
        eq("two links", m.linkify("https://a.org https://b.org"), '<a href="https://a.org">https://a.org</a> <a href="https://b.org">https://b.org</a>')
        eq("an http and an https", m.linkify("http://a.org https://b.org"), 'http://a.org <a href="https://b.org">https://b.org</a>')
        eq("line breaks", m.linkify("a\nhttps://a.org\r\nb"), 'a<br><a href="https://a.org">https://a.org</a><br>b')
        eq("runs of spaces are kept", m.linkify("a   b\n  c"), "a&nbsp;&nbsp; b<br>&nbsp; c")
        eq("a leading space is kept", m.linkify(" a\n b"), "&nbsp;a<br>&nbsp;b")
        eq("no host, no link", m.linkify("https:// and https://."), "https:// and https://.")
        eq("javascript: is text", m.linkify("javascript:alert(1)"), "javascript:alert(1)")
        eq("file: is text", m.linkify("file:///etc/passwd"), "file:///etc/passwd")
        eq("data: is text", m.linkify("data:text/html,<b>x</b>"), "data:text/html,&lt;b&gt;x&lt;/b&gt;")
        eq("ftp: is text", m.linkify("ftp://a.org/x"), "ftp://a.org/x")
        eq("<img> before an http URL", onlyLinks("img before http", "<img src=x>http://a"), "&lt;img src=x&gt;http://a")
        eq("<img> before a link", onlyLinks("img before link", "<img src=x>https://a"),
           '&lt;img src=x&gt;<a href="https://a">https://a</a>')
        eq("a quote cannot close the href", onlyLinks("quote", 'https://a"onmouseover=alert(1)'),
           '<a href="https://a">https://a</a>&quot;onmouseover=alert(1)')
        eq("nor on an http URL", onlyLinks("quote http", 'http://a"onmouseover=alert(1)'), "http://a&quot;onmouseover=alert(1)")
        eq("nor a single quote", onlyLinks("single quote", "https://a'onmouseover=alert(1)"),
           '<a href="https://a">https://a</a>&#39;onmouseover=alert(1)')
        eq("nor a >", onlyLinks("gt", "https://a><img src=https://b/x.png>"),
           '<a href="https://a">https://a</a>&gt;&lt;img src=<a href="https://b/x.png">https://b/x.png</a>&gt;')
        eq("an <a> tag is shown, not used", onlyLinks("anchor", '<a href="javascript:alert(1)">x</a>'),
           '&lt;a href=&quot;javascript:alert(1)&quot;&gt;x&lt;/a&gt;')
        eq("entities stay text", onlyLinks("entities", "https://a/&quot;&lt;img&gt;"),
           '<a href="https://a/&amp;quot;&amp;lt;img&amp;gt">https://a/&amp;quot;&amp;lt;img&amp;gt</a>;')
        onlyLinks("javascript inside a link", "https://a/javascript:alert(1)")
        onlyLinks("unicode", "https://例え.jp/パス?q=値 and «https://a»")
        // What a click may open.
        eq("open https", m.safeUrl("https://a.org/x"), "https://a.org/x")
        eq("open https, & unescaped", m.safeUrl("https://a.org/?x=1&amp;y=2"), "https://a.org/?x=1&y=2")
        eq("refuse http", m.safeUrl("http://a.org/x"), "")
        eq("refuse javascript:", m.safeUrl("javascript:alert(1)"), "")
        eq("refuse file:", m.safeUrl("file:///etc/passwd"), "")
        eq("refuse data:", m.safeUrl("data:text/html,x"), "")
        eq("refuse a scheme with a space", m.safeUrl(" https://a.org"), "")
        eq("refuse no host", m.safeUrl("https://"), "")
        eq("refuse a quote", m.safeUrl('https://a"b'), "")
        eq("refuse an escaped quote", m.safeUrl("https://a&quot;b"), "")
        eq("refuse whitespace", m.safeUrl("https://a b"), "")
        eq("refuse empty", m.safeUrl(""), "")
        // Every href linkify writes, safeUrl accepts, and it is the URL as typed.
        var typed = "https://a.org/p?x=1&y=<2>"
        var href = (m.linkify(typed).match(/href="([^"]*)"/) || [])[1]
        eq("round trip", m.safeUrl(href), "https://a.org/p?x=1&y=")
        // A click opens exactly that URL; a refused one opens nothing.
        backend.opened = []
        m.openLink(href); m.openLink("javascript:alert(1)"); m.openLink("http://a.org")
        eq("a click opens exactly the https URL, nothing else", JSON.stringify(backend.opened), JSON.stringify(["https://a.org/p?x=1&y="]))
        console.log(fails === 0 ? "linkify: all cases pass" : "linkify: " + fails + " FAILED")
        Qt.exit(fails === 0 ? 0 : 1)
    }

    // ── markdown: what a post body may turn into ──────────────────────────────
    function markdownCases(m) {
        var fails = 0
        function eq(name, got, want) {
            if (got !== want) { fails++; console.log("FAIL " + name + "\n  got:  " + got + "\n  want: " + want) }
            else console.log("ok   " + name)
        }
        function ok(name, cond, detail) { if (!cond) { fails++; console.log("FAIL " + name + (detail ? ": " + detail : "")) } else console.log("ok   " + name) }
        var C = { code: "#f5925e", quote: "#8b93a1", dim: "#8b93a1" }
        function r(t) { return Md.render(t, C) }
        // Every href in an output, opened as a click would: exactly an https
        // URL the view's own check accepts, unchanged by it.
        function hrefsOk(out) {
            var re = /<a href="([^"]*)">/g, h
            while ((h = re.exec(out)) !== null) if (m.safeUrl(h[1]) === "" ) return false
            return true
        }
        function safe(name, input) {
            var out = r(input)
            ok(name + " (own tags only)", Md.onlyOwnTags(out) && hrefsOk(out), out)
            return out
        }
        var code = '<font color="#f5925e">', dim = '<font color="#8b93a1">'
        // The subset.
        eq("bold", r("**a** __b__"), "<b>a</b> <b>b</b>")
        eq("italic", r("*a* _b_"), "<i>a</i> <i>b</i>")
        eq("bold italic", r("***a***"), "<i><b>a</b></i>")
        eq("nested emphasis", r("**a *b* c**"), "<b>a <i>b</i> c</b>")
        eq("italic around bold", r("*a **b** c*"), "<i>a <b>b</b> c</i>")
        eq("unclosed stays text", r("**a"), "**a")
        eq("crossing pairs do not cross", r("*a _b* c_"), "<i>a _b</i> c_")
        eq("crossing the other way", r("_a *b_ c*"), "<i>a *b</i> c*")
        eq("snake_case is not italic", r("snake_case_name"), "snake_case_name")
        eq("a lone star is text", r("2 * 3 * 4"), "2 * 3 * 4")
        eq("inline code", r("run `ls -la`"), "run " + code + "ls&nbsp;-la</font>")
        eq("code keeps markup as text", r("`**not bold** <b>`"), code + "**not&nbsp;bold**&nbsp;&lt;b&gt;</font>")
        eq("fenced code", r("```js\nif (a < b) **x**\n```\nafter"), "<pre>if (a &lt; b) **x**</pre>after")
        eq("an unclosed fence runs to the end", r("```\n<img src=x>"), "<pre>&lt;img src=x&gt;</pre>")
        eq("quote", r("> said *this*"), dim + "▎&nbsp;said <i>this</i></font>")
        eq("nested quote", r(">> deeper"), dim + "▎▎&nbsp;deeper</font>")
        eq("list", r("- one\n* two\n  + sub"), "•&nbsp;one<br>•&nbsp;two<br>&nbsp;&nbsp;&nbsp;•&nbsp;sub")
        eq("ordered list", r("1. one\n2) two"), "1.&nbsp;one<br>2)&nbsp;two")
        eq("line breaks kept", r("a\nb"), "a<br>b")
        eq("bare https link", r("see https://a.org/x."), 'see <a href="https://a.org/x">https://a.org/x</a>.')
        eq("link with text shows its host", r("[notes](https://a.org/n)"), '<a href="https://a.org/n">notes</a> ' + dim + "(a.org)</font>")
        eq("link text that looks like another site", r("[https://bank.org](https://evil.org/x)"),
           '<a href="https://evil.org/x">https://bank.org</a> ' + dim + "(evil.org)</font>")
        eq("bold link text", r("[**x**](https://a.org)"), '<a href="https://a.org"><b>x</b></a> ' + dim + "(a.org)</font>")
        eq("& in a link", r("https://a.org/?x=1&y=2"), '<a href="https://a.org/?x=1&amp;y=2">https://a.org/?x=1&amp;y=2</a>')
        eq("a link inside emphasis", r("**https://a.org**"), '<b><a href="https://a.org">https://a.org</a></b>')
        // Raw HTML is shown, never used.
        eq("raw html", safe("raw html", "<b>x</b><script>alert(1)</script>"), "&lt;b&gt;x&lt;/b&gt;&lt;script&gt;alert(1)&lt;/script&gt;")
        eq("img", safe("img", '<img src="https://e.org/x.png" onerror=alert(1)>'), "&lt;img src=&quot;<a href=\"https://e.org/x.png\">https://e.org/x.png</a>&quot; onerror=alert(1)&gt;")
        eq("an img that starts with a link", safe("img link", "<img src=https://e.org/x.png>"), '&lt;img src=<a href="https://e.org/x.png">https://e.org/x.png</a>&gt;')
        eq("image syntax is not an image", safe("image syntax", "![x](https://e.org/x.png)"), '!<a href="https://e.org/x.png">x</a> ' + dim + "(e.org)</font>")
        eq("entities stay as typed", safe("entities", "&#60;img&#62; &lt;b&gt; &amp;"), "&amp;#60;img&amp;#62; &amp;lt;b&amp;gt; &amp;amp;")
        // One backslash, and two.
        eq("backslash before a tag", safe("bs tag", "\\<img src=x>"), "&lt;img src=x&gt;")
        eq("two backslashes before a tag", safe("bs bs tag", "\\\\<img src=x>"), "\\&lt;img src=x&gt;")
        eq("backslash before a quote", safe("bs quote", '\\"'), "&quot;")
        eq("backslash before an ampersand", safe("bs amp", "\\&lt;"), "&amp;lt;")
        eq("escaped stars are text", r("\\*not italic\\*"), "*not italic*")
        eq("escaped backtick", r("\\`not code\\`"), "`not code`")
        eq("escaped bracket is no link", r("\\[x](https://a.org)"), '[x](<a href="https://a.org">https://a.org</a>)')
        eq("backslash before a letter stays", r("a\\b"), "a\\b")
        eq("a trailing backslash stays", r("a\\"), "a\\")
        eq("backslash in a URL ends it", safe("bs url", 'https://a.org/\\"><img src=x>'), '<a href="https://a.org/">https://a.org/</a>&quot;&gt;&lt;img src=x&gt;')
        eq("backslash in a link target refuses it", safe("bs target", "[x](https://a.org/\\x)"), "[x](<a href=\"https://a.org/\">https://a.org/</a>\\x)")
        // Links that are not https, or try to leave the href.
        eq("javascript: link", safe("js", "[x](javascript:alert(1))"), "[x](javascript:alert(1))")
        eq("JavaScript: with spaces", safe("js2", "[x]( javascript:alert(1))"), "[x]( javascript:alert(1))")
        eq("http link", safe("http", "[x](http://a.org)"), "[x](http://a.org)")
        eq("data: link", safe("data", "[x](data:text/html,<b>)"), "[x](data:text/html,&lt;b&gt;)")
        eq("quote in a target", safe("quote target", '[x](https://a.org"onmouseover=alert(1))'), '[x](<a href="https://a.org">https://a.org</a>&quot;onmouseover=alert(1))')
        eq("user info in a target", safe("userinfo", "[x](https://bank.org@evil.org)"), "[x](https://bank.org@evil.org)")
        eq("no host", safe("no host", "[x](https://)"), "[x](https://)")
        eq("a link inside a link", safe("nested link", "[[a](https://b.org)](https://c.org)"),
           '[<a href="https://b.org">a</a> ' + dim + '(b.org)</font>](<a href="https://c.org">https://c.org</a>)')
        eq("no link inside code", safe("code link", "`https://a.org`"), code + "https://a.org</font>")
        safe("html in a link's text", "[<img src=x>](https://a.org)")
        safe("emphasis across a link", "*a [b* c](https://a.org) d*")
        safe("unicode", "**é** _ü_ `ö` https://例え.jp/パス « https://a »")
        safe("control characters", "a\u0000b\u202Ec**d**\u200B")
        // What a click on any of them opens.
        var href = (r("[x](https://a.org/p?q=1&r=2)").match(/href="([^"]*)"/) || [])[1]
        eq("a click opens the URL as typed", m.safeUrl(href), "https://a.org/p?q=1&r=2")

        // Random text made of the characters that matter: always only our
        // own tags, nested, with https hrefs the click check accepts.
        var alphabet = ["*", "_", "**", "__", "`", "```", "\\", "[", "]", "(", ")", "<", ">", "\"", "'", "&", "#", ";", "!",
                        "https://a.org/", "http://", "javascript:", "@", "-", "1.", "> ", "\n", " ", "x", "é", "<img src=x>", "&lt;", "\u202E"]
        var seed = 12345
        function rnd(n) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n }
        var bad = 0
        for (var t = 0; t < 4000; t++) {
            var s = "", len = 1 + rnd(60)
            for (var k = 0; k < len; k++) s += alphabet[rnd(alphabet.length)]
            var out = r(s)
            if (!Md.onlyOwnTags(out) || !hrefsOk(out)) { if (bad++ < 5) console.log("FAIL fuzz input " + JSON.stringify(s) + "\n  out " + out) }
        }
        ok("4000 random inputs: own tags only, https hrefs only", bad === 0, bad + " bad")

        // The largest bodies a post may have (20,000 bytes), built to be the
        // worst case for each rule: each renders, safely, well within a second.
        function rep(x, n) { return new Array(n + 1).join(x) }
        var huge = {
            "stars": rep("*", 20000), "star a": rep("*a", 10000), "underscores": rep("_a", 10000), "bold opens": rep("**a ", 5000),
            "brackets": rep("[", 20000), "open links": rep("[a](", 5000), "link-likes": rep("[a](https://a.org/ ", 1052),
            "backticks": rep("`", 20000), "backtick runs": rep("`a``", 5000), "backslashes": rep("\\", 20000),
            "quotes": rep("> ", 10000), "quote lines": rep(">\n", 10000), "list lines": rep("- *a\n", 5000),
            "fences": rep("```\n", 5000), "urls": rep("https://a.org/", 1428), "one url": "https://a.org/" + rep("x", 19980),
            "tags": rep("<b>", 6666), "entities": rep("&#60;", 4000), "mixed": rep("*_`[\\(<&", 2500)
        }
        for (var name in huge) {
            var t0 = Date.now()
            var o = r(huge[name])
            var ms = Date.now() - t0
            ok("20 kB of " + name + ": " + ms + " ms, own tags only", ms < 1000 && Md.onlyOwnTags(o) && hrefsOk(o), ms + " ms")
        }
        // And Qt lays out the biggest of them without trouble.
        var big = Qt.createQmlObject('import QtQuick; Text { width: 600; wrapMode: Text.WrapAtWordBoundaryOrAnywhere; textFormat: Text.StyledText }', win)
        var t1 = Date.now()
        big.text = r(huge["mixed"]); var h = big.implicitHeight
        ok("Qt lays out 20 kB of rendered Markdown in " + (Date.now() - t1) + " ms", h > 0 && Date.now() - t1 < 3000)
        console.log(fails === 0 ? "markdown: all cases pass" : "markdown: " + fails + " FAILED")
        Qt.exit(fails === 0 ? 0 : 1)
    }

    // ── authors: who the topic list names, where there is no badge ───────────
    function authorCases(m) {
        var fails = 0
        function eq(name, got, want) {
            if (got !== want) { fails++; console.log("FAIL " + name + "\n  got:  " + got + "\n  want: " + want) }
            else console.log("ok   " + name)
        }
        var key = "3694648d41f07a9d3c5e6b1a2f4d8c9e0b7a6f5e4d3c2b1a0f9e8d7c6b5ab945"
        eq("someone's account, by its key", m.listAuthor({ mode: "identity", mine: false, author: "x", authorKey: key }), "3694…b945")
        eq("our account, by its label", m.listAuthor({ mode: "identity", mine: true, author: "Alice", authorKey: key }), "Alice")
        eq("an alias written like that key is marked", m.listAuthor({ mode: "alias", mine: false, author: "3694…b945", authorKey: "ff" + key.substring(2) }), "3694…b945 (alias)")
        eq("an alias called anonymous is marked", m.listAuthor({ mode: "alias", mine: false, author: "anonymous", authorKey: key }), "anonymous (alias)")
        eq("an alias called like our account is marked", m.listAuthor({ mode: "alias", mine: false, author: "Alice", authorKey: key }), "Alice (alias)")
        eq("anonymous", m.listAuthor({ mode: "anonymous", mine: false, author: "anonymous", authorKey: key }), "anonymous")
        console.log(fails === 0 ? "authors: all cases pass" : "authors: " + fails + " FAILED")
        Qt.exit(fails === 0 ? 0 : 1)
    }

    // ── scroll: the thread follows its newest message unless the reader scrolled up ──
    function scrollCases(m) {
        var fails = 0
        var sv = find(m, function (o) { return o.objectName === "threadScroll" })
        var pill = find(m, function (o) { return o.objectName === "newReplies" })
        var f = sv.contentItem
        function atEnd() { return f.contentHeight > f.height && Math.abs(f.contentY - (f.contentHeight - f.height)) < 2 }
        function check(name, cond) { if (!cond) fails++; console.log((cond ? "ok   " : "FAIL ") + name
                                     + "  (y " + Math.round(f.contentY) + " / " + Math.round(f.contentHeight - f.height) + ", unseen " + m.unseen + ")") }
        var steps = [
            function () { check("opening a topic shows its newest message", atEnd()) },
            function () { backend.extra++; backend.postArrived("x1", "t1") },
            function () { check("a reply arriving at the bottom is scrolled to", atEnd()) },
            function () { f.contentY = f.contentHeight - f.height - 50 },   // near, within 80 px
            function () { backend.extra++; backend.postArrived("x2", "t1") },
            function () { check("near the bottom (50 px up) still follows", atEnd()) },
            function () { f.contentY = f.contentHeight - f.height - 150 },  // 150 px up: reading
            function () { backend.extra++; backend.postArrived("x2b", "t1") },
            function () { check("150 px up: not pulled down", f.contentY < f.contentHeight - f.height - 140) },
            function () { m.scrollToEnd() },
            function () { f.contentY = 0 },                               // reading older posts
            function () { backend.extra++; backend.postArrived("x3", "t1") },
            function () { check("scrolled up: not pulled down", f.contentY === 0) },
            function () { check("scrolled up: the pill counts 1 new reply", m.unseen === 1 && pill.visible) },
            function () { backend.extra++; backend.postArrived("x4", "t1") },
            function () { check("scrolled up: 2 new replies, still in place", m.unseen === 2 && f.contentY === 0) },
            function () { m.scrollToEnd() },                              // what the pill does
            function () { check("the pill takes the reader to the end", atEnd() && !pill.visible) },
            function () { f.contentY = 0 },
            function () { m.scrollToEnd(); backend.extra++; m.refreshThread() },   // what Reply does
            function () { check("posting a reply shows it, even from higher up", atEnd()) },
            function () { m.openThread("t1") },
            function () { check("reopening the topic shows its newest message", atEnd() && m.unseen === 0) }
        ]
        var i = 0
        var t = Qt.createQmlObject('import QtQuick; Timer { interval: 250; repeat: true }', win)
        t.triggered.connect(function () {
            if (i < steps.length) { steps[i++](); return }
            t.stop()
            console.log(fails === 0 ? "scroll: all cases pass" : "scroll: " + fails + " FAILED")
            Qt.exit(fails === 0 ? 0 : 1)
        })
        t.start()
    }

    // ── media: images and reactions in the view ─────────────────────────────
    function mediaCases(m) {
        var fails = 0
        function check(name, cond) { if (!cond) fails++; console.log((cond ? "ok   " : "FAIL ") + name) }
        var strips = []
        ;(function walk(o) { if (!o) return; if (o.safe !== undefined && o.images !== undefined) strips.push(o)
                             var k = (o.children || []).concat(o.data || []); for (var i = 0; i < k.length; i++) if (k[i] !== o) walk(k[i]) })(m)
        var s = strips[0]
        check("a post's image strip is shown", strips.length >= 2)
        check("a data:image/jpeg URL from the backend is used", s.safe(Pics.thumb0) === Pics.thumb0)
        check("an http URL is not", s.safe("http://127.0.0.1:8977/x.png") === "")
        check("an https URL is not", s.safe("https://example.org/x.jpg") === "")
        check("a file URL is not", s.safe("file:///etc/passwd") === "")
        check("an SVG data URL is not", s.safe("data:image/svg+xml;base64,PHN2Zz4=") === "")
        check("an HTML data URL is not", s.safe("data:text/html;base64,PGI+") === "")
        check("a data URL with a trailing payload is not", s.safe(Pics.thumb0 + '"><img src=x>') === "")
        var steps = [
            function () { check("only the image already kept on this machine was loaded, without a click", JSON.stringify(backend.loads) === JSON.stringify(["t1/0"])) },
            function () {
                var cell = find(m, function (o) { return o.Accessible !== undefined && o.Accessible.name === "Load image 2" && o.enabled === true })
                cell.clicked(null)
            },
            function () { check("a click loads exactly that image", JSON.stringify(backend.loads) === JSON.stringify(["t1/0", "t1/1"])) },
            function () { check("a loaded image is shown full size", strips[0].states[1] && strips[0].states[1].state === "ready" && strips[0].states[1].full === Pics.full1) },
            function () {
                var r = find(m, function (o) { return o.Accessible !== undefined && o.Accessible.name === "React thumbs up" })
                r.clicked(null)
            },
            function () { check("a reaction is counted and marked as ours", JSON.parse(backend.thread("t1")).topic.reactions[0] === 4 && backend.reacted["t10"] === true) },
            function () {
                var r = find(m, function (o) { return o.Accessible !== undefined && o.Accessible.name === "Remove reaction thumbs up" })
                if (r) r.clicked(null)
            },
            function () { check("clicking it again withdraws it", backend.reacted["t10"] === false) },
            function () {
                var att = find(m, function (o) { return o.objectName === "replyImages" })
                var signAs = find(m, function (o) { return o.objectName === "replyAs" })
                signAs.mode = 2
                var btn = find(att, function (o) { return o.text === "Add image" })
                check("no image can be attached to an anonymous post", btn && btn.enabled === false)
                signAs.mode = 0
                check("…and can again as the account", btn && btn.enabled === true)
            }
        ]
        var i = 0
        var t = Qt.createQmlObject('import QtQuick; Timer { interval: 450; repeat: true }', win)
        t.triggered.connect(function () {
            if (i < steps.length) { steps[i++](); return }
            t.stop()
            console.log(fails === 0 ? "media: all cases pass" : "media: " + fails + " FAILED")
            Qt.exit(fails === 0 ? 0 : 1)
        })
        t.start()
    }

    Timer {
        interval: 400; running: view.status === Loader.Ready
        onTriggered: {
            var m = view.item
            if (win.state === "linkify") { linkifyCases(m); return }
            if (win.state === "authors") { authorCases(m); return }
            if (win.state === "markdown") { markdownCases(m); return }
            var pf = find(m, function (o) { return o.openInvite !== undefined && o.openMuted !== undefined })
            if (win.state === "create") { pf.openCreate(); shot.start(); return }
            if (win.state === "join") { pf.openJoin(); shot.start(); return }
            if (win.state === "invite") { pf.openInvite(backend.currentForum); shot.start(); return }
            if (win.state === "leave") { pf.openLeave(backend.currentForum, "Core contributors"); shot.start(); return }
            if (win.state === "muted") { pf.openMuted(); shot.start(); return }
            if (win.state !== "welcome" && win.state !== "list" && win.state !== "newtopic") m.openThread("t1")
            if (win.state === "list") m.openId = ""
            var replyAs = find(m, function (o) { return o.objectName === "replyAs" })
            if (win.state === "alias") { replyAs.mode = 1; replyAs.alias = "Ghost" }
            if (win.state === "stress") { replyAs.mode = 1; replyAs.alias = "A-forty-character-alias-that-never-ends-x" }
            if (win.state === "inject") m.lastError = 'A post did not go out (<img src="http://127.0.0.1:8977/error.png">); it will be retried.'
            if (win.state === "anonymous") replyAs.mode = 2
            var popups = m.data.filter(function (o) { return o.title !== undefined && o.open !== undefined })
            if (win.state === "newtopic") popups.filter(function (p) { return p.title === "New topic" })[0].open()
            if (win.state === "accounts" || win.state === "backup") popups.filter(function (p) { return p.title === "Accounts" })[0].open()
            if (win.state === "inbox") popups.filter(function (p) { return p.title === "Inbox" })[0].open()
            if (win.state === "backup") m.showPhrase("Alice")
            if (win.state === "scroll") { scrollCases(m); return }
            if (win.state === "media") { mediaCases(m); return }
            if (win.state === "imagefail") {
                // A click on the reply's "Load image"; the download does not match.
                var cell = find(m, function (o) { return o.Accessible !== undefined && o.Accessible.name === "Load image 1" && o.enabled === true
                                                   && o.parent && o.parent.parent && o.parent.parent.parent && o.parent.parent.parent.parent
                                                   && o.parent.parent.parent.parent.post && o.parent.parent.parent.parent.post.id === "r1" })
                if (cell) cell.clicked(null)
            }
            if (win.state === "imagecompose") {
                popups.filter(function (p) { return p.title === "New topic" })[0].open()
                var ti = find(m, function (o) { return o.objectName === "topicImages" })
                ti.add("file:///Users/alice/Pictures/ridge.jpg"); ti.add("file:///Users/alice/Pictures/valley.heic.jpg")
                var tt = find(m, function (o) { return o.placeholderText === "Title" })
                if (tt) tt.text = "Sunset over the ridge"
            }
            if (win.state === "search") {
                var sf = find(m, function (o) { return o.placeholderText === "Search posts on this device" })
                sf.text = "reset"
            }
            if (win.args[3] !== "live") shot.start()
        }
    }
    Timer {
        id: shot; interval: 900
        onTriggered: {
            // For scripts/check-no-remote-fetch.py: the links were really rendered as links.
            var n = 0, hrefs = []
            ;(function walk(o) { if (!o) return
                                 if (o.linked === true) { n++; hrefs = hrefs.concat((o.html.match(/href="[^"]*"/g) || [])) }
                                 var k = o.children || []; for (var i = 0; i < k.length; i++) walk(k[i]) })(view.item)
            console.log("links rendered in " + n + " texts")
            console.log("hrefs " + JSON.stringify(hrefs))
            // A click on the topic's first link, as Text reports it (linkAt under
            // the pointer, then linkActivated): the stand-in backend logs what it is asked to open.
            if (win.state === "inject") {
                var body = find(view.item, function (o) { return o.linked === true && o.raw !== undefined && o.raw.indexOf(" body, see ") > 0 })
                var href = ""
                for (var y = 2; y < body.height && href === ""; y += 4)
                    for (var x = 0; x < body.width && href === ""; x += 4) href = body.linkAt(x, y)
                body.linkActivated(href)
            }
            win.contentItem.grabToImage(function (r) { r.saveToFile(win.args[3] || "/tmp/shot.png"); Qt.quit() })
        }
    }
}
