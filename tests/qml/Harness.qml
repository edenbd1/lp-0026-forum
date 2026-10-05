import QtQuick
import QtQuick.Controls

// Renders the real Main.qml against a stand-in backend, at a given size and in
// a given state, and saves a screenshot. Used to check the layout at every
// width from a phone to a wide desktop without running Basecamp.
//
//   qml tests/qml/Harness.qml -- <width> <height> <state> <out.png>
//   out.png "live" keeps the window open (to resize it by hand) instead of saving.
//   state: welcome | list | thread | alias | anonymous | newtopic | accounts | offline | stress | inject | links
//   (inject: a title, an alias and bodies carrying <img> tags, https links and plain http URLs pointing at 127.0.0.1:8977, none to be fetched)
//   (links: a thread whose posts carry links, for screenshots)
//
//   qml tests/qml/Harness.qml -- 800 600 linkify   unit cases for linkify/safeUrl, exit 1 on any failure
//   qml tests/qml/Harness.qml -- 800 600 scroll    auto-scroll behaviour of the thread, exit 1 on any failure
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
            '<img src="http://127.0.0.1:8977/body.png"> body, see https://127.0.0.1:8977/link.html. Plain http is no link: http://127.0.0.1:8977/plain-http',
            { title: '<img src="http://127.0.0.1:8977/title.png"><b>✓ verified</b> https://127.0.0.1:8977/title-link', replies: 0, last: t0, unread: true })
        // Links that try to smuggle a tag or an attribute in, each one pointing at the check's server.
        readonly property var injectReplies: [
            post("r1", "Mallory", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 3,
                 '<img src=https://127.0.0.1:8977/x.png>https://127.0.0.1:8977/after-img <img src="//127.0.0.1:8977/schemeless.png" width="9" height="9">'),
            post("r2", "Mallory", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 2,
                 'https://127.0.0.1:8977/q"><img src="http://127.0.0.1:8977/quote.png">\nhttps://127.0.0.1:8977/a\'onmouseover=x <a href="https://127.0.0.1:8977/anchor">a</a>'),
            post("r3", "Mallory", "43ca10be77f0e2d9a8b6c4e2f0a1b3c5d7e9f1a3b5c7d9e1f3a5b7c9d1e353a0", "identity", false, 1,
                 'javascript:alert(1) file:///etc/passwd data:text/html,<img src=http://127.0.0.1:8977/data.png> https://127.0.0.1:8977/tls http://127.0.0.1:8977/http')
        ]
        // Scroll state: a long thread that grows on demand.
        property int extra: 0
        readonly property var linkTopic: post("t1", "Ghost", "6d6bb1fa0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d861a", "alias", false, 30,
            "The release notes are at https://github.com/logos-co/logos-basecamp/releases and the testnet status at https://status.logos.co.\nWhat broke for you?",
            { title: "Basecamp 0.3.0: links and notes", replies: 2, last: t0, unread: false })
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
            if (win.state === "inject") return JSON.stringify({ topic: injectTopic, replies: [injectTopic].concat(injectReplies) })
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
        var opened = []
        m.urlOpener = function (u) { opened.push(u) }
        m.openLink(href); m.openLink("javascript:alert(1)"); m.openLink("http://a.org")
        eq("a click opens exactly the https URL, nothing else", JSON.stringify(opened), JSON.stringify(["https://a.org/p?x=1&y="]))
        m.urlOpener = null
        console.log(fails === 0 ? "linkify: all cases pass" : "linkify: " + fails + " FAILED")
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

    Timer {
        interval: 400; running: view.status === Loader.Ready
        onTriggered: {
            var m = view.item
            if (win.state === "linkify") { linkifyCases(m); return }
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
            if (win.state === "scroll") { scrollCases(m); return }
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
            // the pointer, then linkActivated), into a stub that records the URL.
            if (win.state === "inject") {
                var body = find(view.item, function (o) { return o.linked === true && o.raw !== undefined && o.raw.indexOf(" body, see ") > 0 })
                var href = ""
                for (var y = 2; y < body.height && href === ""; y += 4)
                    for (var x = 0; x < body.width && href === ""; x += 4) href = body.linkAt(x, y)
                view.item.urlOpener = function (u) { console.log("click opens " + u) }
                body.linkActivated(href)
                view.item.urlOpener = null
            }
            win.contentItem.grabToImage(function (r) { r.saveToFile(win.args[3] || "/tmp/shot.png"); Qt.quit() })
        }
    }
}
