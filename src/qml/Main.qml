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
    readonly property string rlnJson:       backend ? backend.rlnJson : "{}"
    // RLN membership: {on, phase, title, detail, payer, needs, holds}.
    readonly property var rln: { try { return JSON.parse(rlnJson) } catch (e) { return ({}) } }
    // Posting waits on the membership (the posts are kept, not refused).
    readonly property bool rlnBlocks: rln.on === true && rln.phase !== "active" && rln.phase !== "quota"

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
    readonly property color raised: "#20242b"
    readonly property color line: "#2a2f37"
    readonly property color text: "#e7e9ee"
    readonly property color dim: "#8b93a1"
    readonly property color accent: "#f5925e"      // Logos orange, light: text and markers
    readonly property color focusRing: "#f7b58a"      // light orange: the border of the field being typed in
    readonly property color accentStrong: "#e2552b"   // Logos orange: buttons and selection
    readonly property color ok: "#4cc38a"
    readonly property color warn: "#f2c14e"          // amber, kept apart from the orange accent
    readonly property color link: "#6cb4ff"          // links in posts

    // Every stock control (buttons, combo boxes, fields, spin boxes, popups)
    // reads its colours from the palette, so one dark palette here keeps the
    // whole app consistent instead of restyling each control.
    palette.window: panel
    palette.windowText: text
    palette.base: bg
    palette.alternateBase: panel
    palette.text: text
    palette.button: raised
    palette.buttonText: text
    palette.brightText: "#ffffff"
    palette.highlight: accentStrong
    palette.highlightedText: "#ffffff"
    palette.placeholderText: dim
    palette.mid: line
    palette.dark: accent          // combo-box arrows, same light orange as the bullets
    palette.light: raised
    palette.midlight: line
    palette.shadow: "#000000"
    palette.toolTipBase: raised
    palette.toolTipText: text
    palette.disabled.buttonText: "#5d646f"
    palette.disabled.button: "#191c21"
    palette.disabled.text: "#5d646f"
    palette.disabled.dark: "#3a241c"
    palette.disabled.brightText: "#7d8594"

    // Breakpoints. Below `compact` there is room for one pane only: the topic
    // list, or the open topic with a way back. Below `narrow` the header
    // stacks. Everything else wraps or elides, so no width pushes a control
    // out of view.
    readonly property bool compact: width < 720
    readonly property bool narrow: width < 960
    readonly property bool phone: width < 480
    readonly property int gap: compact ? 10 : 16

    property string search: ""
    function matches(t) {
        if (search.trim() === "") return true
        var q = search.toLowerCase()
        return (t.title + " " + t.body + " " + t.author).toLowerCase().indexOf(q) >= 0
    }
    readonly property var shownTopics: topics.filter(matches)
    readonly property int unreadCount: topics.filter(function (t) { return t.unread }).length

    function log(m) { console.log("[logos_forum qml] " + m) }

    function refreshTopics() {
        if (!ready) return
        logos.watch(backend.listTopics(), function (json) {
            try { root.topics = JSON.parse(json); root.revealIfListed() } catch (e) { log("topics: " + e) }
        }, function (err) { log("listTopics failed: " + err) })
    }
    function refreshThread() {
        if (!ready || openId === "") return
        var asked = openId
        logos.watch(backend.thread(openId), function (json) {
            try {
                var t = JSON.parse(json)
                if (asked !== root.openId) return   // another topic was opened meanwhile
                var replies = t.replies || []
                // Replies that arrive while the reader is scrolled up are
                // counted for the "new replies" pill instead of pulling the view down.
                if (!root.followEnd && replies.length > root.openReplies.length)
                    root.unseen += replies.length - root.openReplies.length
                // Replacing the replies rebuilds them, and the pane briefly
                // shrinks: a reader who scrolled up keeps their place through it.
                if (!root.followEnd) { root.keepY = threadScroll.contentItem.contentY; settle.restart() }
                root.openTopic = t.topic
                root.openReplies = replies
            } catch (e) { log("thread: " + e) }
        }, function (err) { log("thread failed: " + err) })
    }
    function openThread(id) {
        openId = id; openTopic = null; openReplies = []
        followEnd = true; unseen = 0
        refreshThread()
        call(backend.markRead(id), function () { root.refreshTopics() })
    }
    // A topic just posted: scrolled into view in the list once it is listed.
    property string revealTopic: ""
    function revealIfListed() {
        if (revealTopic === "") return
        var i = shownTopics.findIndex(function (t) { return t.id === root.revealTopic })
        if (i >= 0) { topicList.positionViewAtIndex(i, ListView.Contain); revealTopic = "" }
    }
    // The thread keeps to its newest message while the reader is at (or within
    // 80 px of) the bottom; once they scroll up to read, it stays where they are.
    property bool followEnd: true
    property int unseen: 0
    property real keepY: -1
    Timer { id: settle; interval: 300; onTriggered: root.keepY = -1 }
    function scrollToEnd() {
        followEnd = true; unseen = 0; keepY = -1
        var f = threadScroll.contentItem
        f.contentY = Math.max(0, f.contentHeight - f.height)
    }
    // "just now", "5 min ago", "3 h ago", "yesterday 14:02", "12 Sep"
    function when(ms) {
        var d = new Date(ms), n = new Date(root.now), s = (root.now - ms) / 1000
        if (s < 60) return "just now"
        if (s < 3600) return Math.floor(s / 60) + " min ago"
        if (d.toDateString() === n.toDateString()) return Math.floor(s / 3600) + " h ago"
        var y = new Date(root.now - 86400000)
        if (d.toDateString() === y.toDateString()) return "yesterday " + Qt.formatTime(d, "HH:mm")
        return d.getFullYear() === n.getFullYear() ? Qt.formatDate(d, "d MMM") : Qt.formatDate(d, "d MMM yyyy")
    }
    // "3694648d…f586b945" -> "3694…b945", the way wallets show an address.
    function shortKey(k) {
        var h = (k || "").replace("…", "")
        return h.length > 10 ? h.substring(0, 4) + "…" + h.substring(h.length - 4) : h
    }
    function esc(t) { return String(t).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;") }

    // Links in text from the network. The whole text is escaped first, so no
    // tag of its author's can exist. Then only https URLs become
    // <a href="…">…</a>, built here from the escaped URL (plain http stays
    // text: it is not offered as a link), and the result is
    // shown as StyledText (never RichText). So there is no <img> to fetch, and
    // nothing is loaded at all until the reader clicks a link. Line breaks
    // become <br> and runs of spaces &nbsp;, so the text reads as typed.
    function linkify(t) {
        var s = esc(t).replace(/'/g, "&#39;")
        // A URL runs up to whitespace or an escaped < > " ' (in the escaped
        // text, the only & left are the ones escaping starts with).
        var re = /https:\/\/(?:(?!&(?:lt|gt|quot|#39);)[^\s<>"'])+/gi
        var out = "", last = 0, m
        while ((m = re.exec(s)) !== null) {
            var u = m[0]
            // Punctuation that ends a sentence is not part of the URL.
            while (u.length > 0 && ".,:;!?)]}".indexOf(u.charAt(u.length - 1)) >= 0) {
                if (u.charAt(u.length - 1) === ";" && /&amp;$/.test(u)) break   // an escaped &
                u = u.substring(0, u.length - 1)
            }
            if (!/^https:\/\/[^\/?#&.]/i.test(u)) continue   // no host: leave as text
            out += spaces(s.substring(last, m.index), last === 0) + "<a href=\"" + u + "\">" + u + "</a>"
            last = m.index + u.length
            re.lastIndex = last
        }
        return out + spaces(s.substring(last), last === 0)
    }
    // Line breaks and runs of spaces, which StyledText would collapse.
    function spaces(s, lineStart) {
        s = s.replace(/\r\n|\r|\n/g, "<br>").replace(/\t/g, "    ").replace(/ (?= )/g, "&nbsp;")
        return s.replace(lineStart ? /(^|<br>) /g : /(<br>) /g, "$1&nbsp;")
    }
    // What a clicked link opens: only an https URL as linkify wrote
    // it (the href arrives still escaped), anything else opens nothing.
    function safeUrl(href) {
        var u = String(href || "")
        if (!/^https:\/\/(?:(?!&(?:lt|gt|quot|#39);)[^\s<>"'])+$/i.test(u)) return ""
        u = u.replace(/&amp;/g, "&")
        return /^https:\/\/[^\/?#&.]/i.test(u) ? u : ""
    }
    // Set only by tests/qml/Harness.qml, to see what a click would open.
    property var urlOpener: null
    function openLink(href) {
        var u = safeUrl(href)
        if (u === "") { log("refused to open a link: " + href); return }
        if (urlOpener) urlOpener(u)
        else Qt.openUrlExternally(u)
    }
    function exact(ms) { return Qt.formatDateTime(new Date(ms), "d MMM yyyy, HH:mm") }
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
        // topicId "" means many posts at once (a history import): refresh once.
        function onPostArrived(id, topicId) {
            root.refreshTopics()
            if (root.openId !== "" && (topicId === root.openId || topicId === "")) {
                root.refreshThread()
                root.call(root.backend.markRead(root.openId))  // the user is looking at it
            }
        }
        function onPostStateChanged(id, state, detail) {
            root.refreshTopics()
            root.refreshThread()
            // Without an RLN membership every send times out: the banner says why.
            if (state === "failed" && !root.rlnBlocks) root.lastError = "A post did not go out (" + detail + "); it will be retried."
        }
        function onOutboxCountChanged() { root.refreshTopics(); root.refreshThread() }
    }
    Component.onCompleted: {
        ready = backend !== null && logos.isViewModuleReady("logos_forum")
        try { accounts = JSON.parse(accountsJson) } catch (e) {}
    }

    Rectangle { anchors.fill: parent; color: root.bg }

    // ── Reusable pieces ───────────────────────────────────────────────────────

    // Plain text by default: titles, aliases and author names come from other
    // people, and rich text would render an <img> that fetches a URL, telling
    // its author the reader's IP address.
    component Label2: Text { color: root.text; font.pixelSize: 14; wrapMode: Text.WrapAtWordBoundaryOrAnywhere; textFormat: Text.PlainText }
    component Dim: Text { color: root.dim; font.pixelSize: 12; wrapMode: Text.WrapAtWordBoundaryOrAnywhere; textFormat: Text.PlainText }
    // Text from the network with its https links clickable (see linkify).
    // Text without a link stays plain text, exactly as before.
    component LinkText: Text {
        id: lt
        property string raw: ""
        readonly property string html: root.linkify(raw)
        readonly property bool linked: html.indexOf("<a href=") >= 0
        text: linked ? html : raw
        textFormat: linked ? Text.StyledText : Text.PlainText
        color: root.text; font.pixelSize: 14; wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        linkColor: root.link
        onLinkActivated: function (href) { root.openLink(href) }
        HoverHandler { cursorShape: lt.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor }
        ToolTip.visible: lt.hoveredLink !== ""
        ToolTip.delay: 400
        ToolTip.text: root.safeUrl(lt.hoveredLink)
    }
    // A label between controls in a Flow: centred on the 40 px control height.
    component FlowDim: Dim { height: 40; verticalAlignment: Text.AlignVCenter; wrapMode: Text.NoWrap }

    // Who a post is by, and how it was signed. The key is the one the signature
    // was checked against; the badge says whether it is an account, an alias
    // or a one-time anonymous key.
    component Byline: Flow {   // wraps onto a second line in a narrow pane
        property var post
        spacing: 6
        Text {
            // Someone else's account is only known by its key, shown wallet-style.
            readonly property bool keyIsName: post && post.mode === "identity" && !post.mine
            text: post ? (keyIsName ? root.shortKey(post.authorKey) : post.author) : ""
            textFormat: Text.PlainText
            color: post && post.mode === "anonymous" ? root.dim : root.accent
            font.pixelSize: 13; font.bold: true
        }
        Rectangle {
            // Aliases are marked as such; an anonymous post's name already says it.
            visible: post && post.mode === "alias"
            radius: 3; color: root.line
            implicitWidth: badge.implicitWidth + 8; implicitHeight: badge.implicitHeight + 2
            Text { id: badge; anchors.centerIn: parent; color: root.dim; font.pixelSize: 10
                   text: "alias" }
        }
        Text {
            text: post ? (post.mode === "identity" && !post.mine ? "✓ verified" : "✓ " + post.authorKey.substring(0, 8)) : ""
            color: root.dim; font.pixelSize: 11; font.family: "monospace"
            ToolTip.visible: keyHover.hovered
            ToolTip.text: post ? "Signature verified against key " + post.authorKey : ""
            HoverHandler { id: keyHover }
        }
        Dim {
            text: post ? "· " + root.when(post.ts) : ""
            ToolTip.visible: timeHover.hovered; ToolTip.text: post ? root.exact(post.ts) : ""
            HoverHandler { id: timeHover }
        }
        Text {
            visible: post && post.state !== ""
            // A post the node accepted but the network has not confirmed stays
            // "sending" for a few seconds; after that, say what is going on.
            text: !post ? ""
                : root.rlnBlocks ? "· waiting for an RLN membership"
                : root.rln.phase === "quota" ? "· held by the rate limit, goes out next epoch"
                : (post.state === "sending" && root.now - post.ts < 30000) ? "· sending…"
                : "· waiting for the network, will retry"
            color: root.warn; font.pixelSize: 12
        }
    }

    // The main action button: a dark button outlined in a bright version of
    // the icon's gradient (orange to gold), with white text.
    component AccentButton: Button {
        id: ab
        HoverHandler { cursorShape: ab.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        contentItem: Text {
            text: ab.text; font: ab.font
            color: ab.enabled ? "#ffffff" : root.dim
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            implicitWidth: 100; implicitHeight: 40
            radius: 7
            color: root.line
            opacity: ab.down ? 0.8 : 1.0
            gradient: ab.enabled ? rim : null
            Gradient {
                id: rim
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#ee5a2c" }
                GradientStop { position: 0.1; color: "#f07a42" }
                GradientStop { position: 0.55; color: "#f29a58" }
                GradientStop { position: 1.0; color: "#f3c46c" }
            }
            Rectangle {
                anchors.fill: parent; anchors.margins: 2; radius: 5
                color: root.panel
            }
            Rectangle {
                visible: ab.enabled && 0.0 > 0
                anchors.fill: parent; anchors.margins: 2; radius: 5
                opacity: 0.0
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#ee5a2c" }
                GradientStop { position: 0.1; color: "#f07a42" }
                GradientStop { position: 0.55; color: "#f29a58" }
                GradientStop { position: 1.0; color: "#f3c46c" }
                }
            }
        }
    }

    // Fields and pickers: the same 6 px corners and dark fill as the text areas.
    component AppField: TextField {
        id: tf
        color: root.text; placeholderTextColor: root.dim
        background: Rectangle { implicitHeight: 40; radius: 6; color: root.bg; border.color: tf.activeFocus ? root.focusRing : root.line }
    }
    component AppCombo: ComboBox {
        id: cb
        HoverHandler { cursorShape: cb.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        background: Rectangle { implicitWidth: 120; implicitHeight: 40; radius: 6; color: cb.down ? root.line : root.raised; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.06) }
        popup.palette: root.palette
    }

    // Every other button: the same 6 px corners, raised grey.
    component AppButton: Button {
        id: nb
        HoverHandler { cursorShape: nb.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        contentItem: Text {
            text: nb.text; font: nb.font; color: nb.enabled ? root.text : root.dim
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            implicitWidth: 80; implicitHeight: 40; radius: 6
            color: nb.down ? root.line : root.raised
            border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.06)
        }
    }

    // Choose how to sign the next post.
    component SignAs: ColumnLayout {
        property alias mode: modeBox.currentIndex
        property alias alias: aliasField.text
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Dim { text: "Post as"; wrapMode: Text.NoWrap }
            AppCombo {
                id: modeBox
                model: [root.myLabel || "My account", "Alias", "Anonymous"]
                implicitWidth: 126
            }
            AppField {
                id: aliasField
                visible: modeBox.currentIndex === 1
                placeholderText: "Name to show"
                maximumLength: 40
                // 150 when there is room, down to 80 in a narrow pane
                Layout.fillWidth: true; Layout.preferredWidth: 150; Layout.maximumWidth: 150; Layout.minimumWidth: 80
            }
            Item { Layout.fillWidth: true }
        }
        // What the others will see, and what it means for privacy. The label
        // ("Account 1") is local; peers only ever see the key. Its own line,
        // so it is read in full at any width.
        Dim {
            Layout.fillWidth: true
            text: modeBox.currentIndex === 2
                ? "Shown as “anonymous”. One-time key: can’t be linked to you."
                : modeBox.currentIndex === 1
                ? "Shown as “" + (aliasField.text.trim() || "…") + "”, signed by your key."
                : "Others see your key " + root.shortKey(root.myKey) + ". Your posts are linked."
        }
    }

    // ── Layout ────────────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.gap
        spacing: root.compact ? 8 : 12

        // Header: forum name on the left, accounts on the right; stacked when narrow.
        GridLayout {
            Layout.fillWidth: true
            columns: root.narrow ? 1 : 2
            columnSpacing: 12; rowSpacing: 8
            ColumnLayout {
                Layout.fillWidth: true; Layout.minimumWidth: 0
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    // The name keeps its full width and only elides when the row is
                    // too short for it; the version takes whatever is left.
                    Text { id: titleText
                           Layout.maximumWidth: Math.ceil(titleMetrics.advanceWidth) + 1; Layout.minimumWidth: 0; Layout.fillWidth: true; elide: Text.ElideRight
                           text: root.forumName; textFormat: Text.PlainText; color: root.text; font.pixelSize: root.compact ? 18 : 20; font.bold: true
                           TextMetrics { id: titleMetrics; font: titleText.font; text: titleText.text } }
                    Dim { Layout.fillWidth: true; Layout.minimumWidth: implicitWidth; text: "v" + root.appVersion; wrapMode: Text.NoWrap }
                }
                Dim { Layout.fillWidth: true; wrapMode: root.phone ? Text.Wrap : Text.NoWrap; elide: Text.ElideRight
                      text: "No server · every post signed and verified · Logos Delivery + Logos Storage" }
            }
            RowLayout {
                Layout.fillWidth: root.narrow
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                spacing: 10
                AppCombo {
                    id: accountBox
                    Layout.fillWidth: root.phone
                    Layout.preferredWidth: 192; Layout.minimumWidth: 110
                    model: root.accounts.map(function (a) { return a.label + " · " + root.shortKey(a.key) })
                    currentIndex: root.accounts.findIndex(function (a) { return a.selected })
                    onActivated: function (i) { root.call(root.backend.selectAccount(root.accounts[i].label), root.plain) }
                }
                AppButton { text: "Accounts…"; onClicked: accountsDialog.open() }
                Item { visible: root.narrow && !root.phone; Layout.fillWidth: true }
            }
        }
        // Node status, what is waiting to go out, and where history came from.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                Layout.alignment: Qt.AlignTop; Layout.topMargin: 4
                width: 8; height: 8; radius: 4
                color: root.status.indexOf("fail") < 0 && root.status.indexOf("Connected") === 0 ? root.ok
                     : root.status.indexOf("fail") >= 0 ? "#e5484d" : root.warn
            }
            Dim {
                Layout.fillWidth: true
                // Styled for the orange count only: both status strings can carry
                // text from the network (a store node's error), so they are escaped.
                textFormat: Text.StyledText
                text: root.esc(root.status)
                      + (root.outboxCount > 0 ? " · <font color=\"" + root.warn + "\">" + root.outboxCount + " waiting to send</font>" : "")
                      + (root.historyStatus !== "" ? " · " + root.esc(root.historyStatus) : "")
            }
        }
        // What posting on an RLN network is waiting for, and what to do about
        // it. Reading never waits on it.
        Rectangle {
            Layout.fillWidth: true
            visible: root.rln.on === true && root.rln.phase !== "active"
            color: root.raised; radius: 6; border.color: root.rln.phase === "quota" ? root.line : root.warn
            implicitHeight: rlnCol.implicitHeight + 20
            ColumnLayout {
                id: rlnCol
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10
                spacing: 6
                Label2 { Layout.fillWidth: true; text: root.rln.title || ""; font.bold: true; color: root.warn }
                Dim { Layout.fillWidth: true; text: root.rln.detail || ""; font.pixelSize: 13 }
                AppButton {
                    visible: root.rln.retry === true
                    text: "Try again now"
                    onClicked: root.call(root.backend.retryMembership())
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: (root.rln.payer || "") !== ""
                    spacing: 8
                    Dim { text: "RLN account"; wrapMode: Text.NoWrap }
                    // Selectable, so it can be copied into a wallet.
                    TextEdit {
                        id: payerField
                        Layout.fillWidth: true
                        text: root.rln.payer || ""
                        readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText
                        color: root.text; selectionColor: root.accentStrong
                        font.family: "monospace"; font.pixelSize: 12
                        wrapMode: TextEdit.WrapAnywhere
                    }
                    AppButton {
                        text: "Copy"
                        onClicked: { payerField.selectAll(); payerField.copy(); payerField.deselect() }
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true; visible: root.lastError !== ""
            text: root.lastError; textFormat: Text.PlainText; color: root.warn; font.pixelSize: 13; wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // Topics
            Rectangle {
                visible: !root.compact || root.openId === ""
                Layout.fillWidth: root.compact
                Layout.preferredWidth: root.compact ? -1 : Math.min(420, Math.max(300, root.width * 0.34))
                Layout.fillHeight: true
                color: root.panel; radius: 6; border.color: root.line

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text { text: "Topics"; color: root.text; font.pixelSize: 15; font.bold: true }
                        Rectangle {
                            visible: root.unreadCount > 0
                            radius: 8; color: root.accentStrong
                            implicitWidth: unreadText.implicitWidth + 12; implicitHeight: 18
                            Text { id: unreadText; anchors.centerIn: parent; color: "white"; font.pixelSize: 11; font.bold: true
                                   text: root.unreadCount + " new" }
                        }
                        Item { Layout.fillWidth: true }
                        AppButton { text: "↻"; implicitWidth: 40
                                 onClicked: root.call(root.backend.catchUp(), function () { root.refreshTopics() })
                                 ToolTip.visible: hovered; ToolTip.text: "Fetch what was posted while you were away" }
                        AccentButton { text: "New topic"; onClicked: newTopic.open() }
                    }
                    AppField {
                        id: searchField
                        Layout.fillWidth: true
                        placeholderText: "Search topics"
                        onTextChanged: root.search = text
                    }
                    ListView {
                        id: topicList
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true; spacing: 4
                        model: root.shownTopics
                        onCountChanged: root.revealIfListed()
                        delegate: Rectangle {
                            width: ListView.view.width
                            height: col.implicitHeight + 16
                            radius: 4
                            color: modelData.id === root.openId ? root.line : "transparent"
                            // Screen readers (and UI tests) see each row as a button named after the topic.
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.title
                            Accessible.onPressAction: root.openThread(modelData.id)
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.openThread(modelData.id) }
                            ColumnLayout {
                                id: col
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter; anchors.margins: 8
                                spacing: 3
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 6
                                    Rectangle { visible: modelData.unread; width: 7; height: 7; radius: 4; color: root.accent }
                                    Label2 { Layout.fillWidth: true; text: modelData.title; font.bold: true; maximumLineCount: 2; elide: Text.ElideRight }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.body.replace(/\s+/g, " ")
                                    color: root.dim; font.pixelSize: 12; elide: Text.ElideRight; maximumLineCount: 1
                                    textFormat: Text.PlainText
                                }
                                Dim { Layout.fillWidth: true; elide: Text.ElideRight; maximumLineCount: 1
                                      text: (modelData.mode === "identity" && !modelData.mine ? root.shortKey(modelData.authorKey) : modelData.author)
                                            + " · " + modelData.replies + (modelData.replies === 1 ? " reply" : " replies")
                                            + " · " + root.when(modelData.last) }
                                Dim { visible: modelData.state !== ""; color: root.warn; text: "not sent yet, will retry" }
                            }
                        }
                        Dim { anchors.centerIn: parent; visible: root.topics.length > 0 && root.shownTopics.length === 0
                              width: parent.width - 32; horizontalAlignment: Text.AlignHCenter
                              text: "No topic matches \"" + root.search + "\"." }
                        Dim { anchors.centerIn: parent; width: parent.width - 32; horizontalAlignment: Text.AlignHCenter
                              visible: root.topics.length === 0
                              text: "No topics yet. Start one, or wait a moment: when you join, peers who were here before you send their history." }
                    }
                }
            }

            // Thread
            Rectangle {
                visible: !root.compact || root.openId !== ""
                Layout.fillWidth: true; Layout.fillHeight: true
                color: root.panel; radius: 6; border.color: root.line

                Flickable {
                    id: welcomeScroll
                    anchors.fill: parent
                    visible: root.openId === ""
                    // Centred in the pane when it fits, scrollable when it does not.
                    contentHeight: Math.max(height, welcome.implicitHeight + 60)
                    clip: true
                    ColumnLayout {
                        id: welcome
                        width: Math.min(welcomeScroll.width - 60, 560)
                        x: (welcomeScroll.width - width) / 2
                        y: Math.max(30, (welcomeScroll.height - implicitHeight) / 2)
                        spacing: 14
                        Text { Layout.fillWidth: true; wrapMode: Text.WrapAtWordBoundaryOrAnywhere; textFormat: Text.PlainText
                               text: "Welcome to " + root.forumName; color: root.text; font.pixelSize: 18; font.bold: true }
                        Label2 { Layout.fillWidth: true; color: root.dim
                                 text: "There is no server here. Posts travel peer to peer over Logos Delivery, history is shared through Logos Storage, and every post is signed. The ✓ next to an author means this app checked the signature itself." }
                        Repeater {
                            model: [
                                ["Your account", "Posts are signed by your account's key, so they link to each other. You can hold several accounts and rotate a key at any time."],
                                ["Alias", "Pick a name for one post. It is still signed by your account, and marked as an alias."],
                                ["Anonymous", "A key made for that one post and then wiped. Two anonymous posts cannot be linked to each other or to you."]
                            ]
                            delegate: RowLayout {
                                Layout.fillWidth: true; spacing: 10
                                Rectangle { Layout.alignment: Qt.AlignTop; Layout.topMargin: 6; width: 6; height: 6; radius: 3; color: root.accent }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 2
                                    Label2 { text: modelData[0]; font.bold: true }
                                    Dim { Layout.fillWidth: true; text: modelData[1]; font.pixelSize: 13 }
                                }
                            }
                        }
                        Dim { Layout.fillWidth: true; text: "Pick a topic on the left, or start one." }
                        AccentButton { text: "New topic"; onClicked: newTopic.open() }
                    }
                }

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: root.compact ? 10 : 14; spacing: 10
                    visible: root.openId !== ""

                    // One pane at a time: the way back to the list.
                    AppButton {
                        visible: root.compact
                        text: root.unreadCount > 0 ? "‹ Topics (" + root.unreadCount + " new)" : "‹ Topics"
                        onClicked: { root.openId = ""; root.openTopic = null; root.openReplies = [] }
                    }

                    Item {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        ScrollView {
                            id: threadScroll
                            objectName: "threadScroll"
                            anchors.fill: parent
                            clip: true
                            contentWidth: availableWidth   // wrap to the pane, never scroll sideways
                            ColumnLayout {
                                width: threadScroll.availableWidth
                                spacing: 14
                                LinkText {
                                    Layout.fillWidth: true
                                    raw: root.openTopic ? root.openTopic.title : "Topic not received yet. Its replies are shown below."
                                    font.pixelSize: 18; font.bold: true
                                }
                                Byline { post: root.openTopic; visible: root.openTopic !== null; Layout.fillWidth: true }
                                LinkText { Layout.fillWidth: true; raw: root.openTopic ? root.openTopic.body : "" }
                                Rectangle { Layout.fillWidth: true; height: 1; color: root.line }
                                Repeater {
                                    model: root.openReplies
                                    delegate: ColumnLayout {
                                        Layout.fillWidth: true; spacing: 4
                                        Byline { post: modelData; Layout.fillWidth: true }
                                        LinkText { Layout.fillWidth: true; raw: modelData.body }
                                    }
                                }
                            }
                        }
                        // Where the reader is: following the newest message when at
                        // (or within 80 px of) the bottom. Scrolling there resumes it.
                        Connections {
                            target: threadScroll.contentItem
                            function onContentYChanged() {
                                if (root.keepY >= 0) return   // the replies are being rebuilt
                                var f = threadScroll.contentItem
                                root.followEnd = f.contentY >= f.contentHeight - f.height - 80
                                if (root.followEnd) root.unseen = 0
                            }
                            function onContentHeightChanged() {
                                var f = threadScroll.contentItem
                                if (root.followEnd) root.scrollToEnd()
                                else if (root.keepY >= 0) f.contentY = Math.max(0, Math.min(root.keepY, f.contentHeight - f.height))
                            }
                            function onHeightChanged() { if (root.followEnd) root.scrollToEnd() }
                        }
                        // Replies that came in below while the reader was scrolled up.
                        Rectangle {
                            objectName: "newReplies"
                            visible: root.unseen > 0 && !root.followEnd
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 8
                            radius: height / 2; color: root.accentStrong
                            implicitWidth: pillText.implicitWidth + 24; implicitHeight: 28
                            Text { id: pillText; anchors.centerIn: parent; color: "white"; font.pixelSize: 12; font.bold: true
                                   text: root.unseen + (root.unseen === 1 ? " new reply ↓" : " new replies ↓") }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.scrollToEnd() }
                        }
                    }

                    // Reply: the picker and its explanation, and the button beside
                    // them, or under them when the pane is too narrow for both.
                    ColumnLayout {
                        id: replyArea
                        Layout.fillWidth: true; spacing: 8
                        TextArea {
                            id: replyBody
                            Layout.fillWidth: true; Layout.preferredHeight: root.compact ? 64 : 80
                            placeholderText: "Write a reply"; wrapMode: TextArea.WrapAtWordBoundaryOrAnywhere
                            color: root.text; placeholderTextColor: root.dim
                            background: Rectangle { color: root.bg; radius: 6; border.color: replyBody.activeFocus ? root.focusRing : root.line }
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: replyArea.width < 470 ? 1 : 2
                            columnSpacing: 12; rowSpacing: 8
                            SignAs { id: replyAs; objectName: "replyAs"; Layout.fillWidth: true; Layout.alignment: Qt.AlignTop }
                            RowLayout {
                                Layout.alignment: Qt.AlignRight | Qt.AlignTop
                                spacing: 8
                                Dim { visible: replyBody.length > 16000; color: replyBody.length > 20000 ? "#e5484d" : root.dim
                                      text: replyBody.length + " / 20000"; wrapMode: Text.NoWrap }
                                AccentButton {
                                    text: "Reply"
                                    enabled: replyBody.text.trim().length > 0
                                    onClicked: root.call(root.backend.reply(root.openId, replyBody.text, replyAs.mode, replyAs.alias), function (r) {
                                        if (root.result(r)) {
                                            replyBody.text = ""
                                            root.scrollToEnd()   // show the reply just posted
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
    }

    // ── Dialogs ───────────────────────────────────────────────────────────────

    Dialog {
        id: newTopic
        palette: root.palette   // popups take their palette from the window, not from us
        background: Rectangle { color: root.panel; radius: 8; border.color: root.line }
        header: Text { text: newTopic.title; color: root.text; font.pixelSize: 16; font.bold: true; padding: 16 }
        title: "New topic"
        modal: true; anchors.centerIn: parent
        width: Math.min(640, root.width - 24)
        footer: Item { implicitHeight: 12 }
        ColumnLayout {
            anchors.fill: parent; spacing: 8
            AppField { id: topicTitle; Layout.fillWidth: true; placeholderText: "Title"; maximumLength: 200 }
            TextArea { id: topicBody; Layout.fillWidth: true; Layout.preferredHeight: 160; placeholderText: "What do you want to say?"; wrapMode: TextArea.WrapAtWordBoundaryOrAnywhere
                            color: root.text; placeholderTextColor: root.dim
                            background: Rectangle { color: root.bg; radius: 6; border.color: topicBody.activeFocus ? root.focusRing : root.line } }
            Dim { Layout.alignment: Qt.AlignRight; visible: topicBody.length > 16000 || topicTitle.length > 160
                  color: topicBody.length > 20000 ? "#e5484d" : root.dim
                  text: "title " + topicTitle.length + " / 200 · body " + topicBody.length + " / 20000" }
            SignAs { id: topicAs; Layout.fillWidth: true }
            RowLayout {
              Layout.alignment: Qt.AlignRight
              spacing: 10
              AppButton { text: "Cancel"; onClicked: newTopic.close() }
              AccentButton {
                text: "Post"
                enabled: topicTitle.text.trim().length > 0 && topicBody.text.trim().length > 0
                onClicked: root.call(root.backend.createTopic(topicTitle.text, topicBody.text, topicAs.mode, topicAs.alias), function (r) {
                    if (root.result(r)) {
                        topicTitle.text = ""; topicBody.text = ""
                        newTopic.close()
                        // Open it, and show it in the list: a search that hides it is cleared.
                        searchField.text = ""; root.revealTopic = r
                        root.openThread(r); root.refreshTopics()
                    }
                })
              }
            }
        }
    }

    Dialog {
        id: accountsDialog
        palette: root.palette
        background: Rectangle { color: root.panel; radius: 8; border.color: root.line }
        header: Text { text: accountsDialog.title; color: root.text; font.pixelSize: 16; font.bold: true; padding: 16 }
        title: "Accounts"
        modal: true; anchors.centerIn: parent
        width: Math.min(820, root.width - 24)
        height: Math.min(implicitHeight, root.height - 24)
        footer: Item {
            implicitHeight: 56
            AppButton { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter
                     text: "Close"; onClicked: accountsDialog.close() }
        }
        property var rot: { try { return JSON.parse(root.rotationJson) } catch (e) { return {} } }
        ScrollView {
          id: accountsScroll
          anchors.fill: parent
          contentWidth: availableWidth
          clip: true
          ColumnLayout {
            width: accountsScroll.availableWidth; spacing: 10
            Repeater {
                model: root.accounts
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 2
                        RowLayout {
                            Layout.fillWidth: true; spacing: 8
                            Label2 { id: accLabel; text: modelData.label; font.bold: modelData.selected
                                     Layout.maximumWidth: Math.ceil(accMetrics.advanceWidth) + 1; Layout.fillWidth: true
                                     elide: Text.ElideRight; wrapMode: Text.NoWrap
                                     TextMetrics { id: accMetrics; font: accLabel.font; text: accLabel.text } }
                            Dim { visible: modelData.selected; text: "in use"; color: root.ok; wrapMode: Text.NoWrap; Layout.fillWidth: true }
                        }
                        Dim { text: root.shortKey(modelData.key) + " · " + modelData.posts + " posts on this key"; Layout.fillWidth: true }
                    }
                    // Only the actions that do something are shown.
                    AppButton { visible: !modelData.selected; text: "Use"; onClicked: root.call(root.backend.selectAccount(modelData.label), root.plain) }
                    AppButton { visible: root.accounts.length > 1; text: "Delete"; onClicked: root.call(root.backend.deleteAccount(modelData.label), root.plain) }
                }
            }
            RowLayout {
                AppField { id: newLabel; placeholderText: "new account name"; Layout.fillWidth: true }
                AppButton { text: "Create"; onClicked: root.call(root.backend.createAccount(newLabel.text), function (r) { if (root.plain(r)) newLabel.text = "" }) }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.line }
            Label2 { text: "Identity rotation"; font.bold: true }
            Dim { Layout.fillWidth: true
                  text: "Replace the selected account's key with a fresh one. Nothing links the old key to the new one, so earlier posts stop being attributable to what you post next." }
            AppButton { text: "Rotate now"; onClicked: root.call(root.backend.rotateAccount(), root.plain) }
            Flow {
                Layout.fillWidth: true; spacing: 8
                FlowDim { text: "Automatically after" }
                SpinBox { id: rotPosts; implicitWidth: 120; from: 0; to: 10000; value: accountsDialog.rot.maxPosts || 0; editable: true }
                FlowDim { text: "posts or" }
                SpinBox { id: rotDays; implicitWidth: 120; from: 0; to: 3650; value: accountsDialog.rot.maxDays || 0; editable: true }
                FlowDim { text: "days" }
                AppButton { text: "Save"; onClicked: root.call(root.backend.setRotation(rotPosts.value, rotDays.value), root.plain) }
            }
            Dim { text: "0 means never." }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.line }
            Label2 { text: "History"; font.bold: true }
            Flow {
                Layout.fillWidth: true; spacing: 8
                AppButton { text: "Fetch missed posts"; onClicked: root.call(root.backend.catchUp()) }
                AppButton { text: "Save snapshot to Logos Storage"; onClicked: root.call(root.backend.saveSnapshot(), root.plain) }
            }
          }
        }
    }
}
