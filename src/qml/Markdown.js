.pragma library
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// A small Markdown subset for post bodies, rendered by this file alone into
// StyledText whose every tag is written here: nothing the author typed is ever
// a tag, an attribute or an entity.
//
//   **bold** __bold__ *italic* _italic_ `code`, ``` fenced code ```,
//   > quotes (up to three levels), - * + and 1. lists, https links (bare, or
//   [text](https://…), shown with the link's host), \ escapes for punctuation.
//
// How it stays safe:
//   - The text is cut into tokens first (text, code, links, runs of * and _).
//     Every character of a text or code token is escaped on output (& < > " '),
//     so a < from the author, also one written as \<, is shown, never parsed;
//     & is escaped too, so &#60; or &lt; are shown as typed.
//   - The only tags produced are <b> <i> <br> <pre> <a href="…"> and
//     <font color="#rrggbb"> with this file's own colours, opened and closed
//     here in nested order (emphasis is matched with two stacks; an opener
//     left inside a closed pair turns back into text).
//   - A link is made only from an https URL with a host, stopped at
//     whitespace, quotes, angle brackets, backslash and backtick, up to 2048
//     characters. Other schemes (javascript:, data:, http:) stay text. There is
//     no image syntax: ![x](…) is a "!" and a link, and no tag can load
//     anything. Clicking a link goes through the backend, as before.
//   - Linear in the size of the text: brackets, parentheses and code-span
//     closers are found from tables built once per line.

var kMaxUrl = 2048

function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                    .replace(/"/g, "&quot;").replace(/'/g, "&#39;")
}
// Spaces StyledText would collapse: runs, and one at the start of a line.
function keepSpaces(s, lineStart) {
    s = s.replace(/\t/g, "    ").replace(/ (?= )/g, "&nbsp;")
    return lineStart ? s.replace(/^ /, "&nbsp;") : s
}
function colour(c, fallback) { return /^#[0-9a-fA-F]{6}$/.test(String(c)) ? String(c) : fallback }

var PUNCT = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
function isPunct(c) { return c !== "" && PUNCT.indexOf(c) >= 0 }
function isSpace(c) { return c === "" || /\s/.test(c) }

// An https URL that may become a link: scheme, then a host that does not start
// with a separator, no user info, nothing that could leave an attribute.
function urlOk(u) {
    if (u.length > kMaxUrl || !/^https:\/\/[^\/?#&.\-:@]/i.test(u)) return false
    if (/[\s<>"'\\`]/.test(u)) return false
    var authority = u.substring(8).split(/[\/?#]/)[0]
    return authority.indexOf("@") < 0
}
function hostOf(u) { return u.substring(8).split(/[\/?#]/)[0] }

// The longest URL at `i` (which starts "https://"), without the punctuation
// that ends a sentence.
function urlAt(line, i) {
    var j = i + 8
    while (j < line.length && !/[\s<>"'\\`]/.test(line.charAt(j))) j++
    var u = line.substring(i, j)
    while (u.length > 8 && ".,:;!?)]}*_".indexOf(u.charAt(u.length - 1)) >= 0) u = u.substring(0, u.length - 1)
    return u
}

// For each position, the index of the next character in `set` at or after it.
function nextOf(line, set) {
    var next = new Array(line.length + 1)
    next[line.length] = -1
    for (var i = line.length - 1; i >= 0; i--) next[i] = set.indexOf(line.charAt(i)) >= 0 ? i : next[i + 1]
    return next
}

// Tokens of one line: {t:"text", s}, {t:"code", s}, {t:"link", href, label:[tokens]},
// {t:"delim", ch, n, rem, open, close, opens:[], closes:[]}.
function tokens(line, allowLinks) {
    var out = [], text = ""
    var brackets = allowLinks ? nextOf(line, "[]") : null
    var parens = allowLinks ? nextOf(line, ") \t") : null
    // Backtick runs, by length, in order: a code span closes at the next run
    // of the same length.
    var runs = {}
    for (var r = 0; r < line.length;) {
        if (line.charAt(r) !== "`") { r++; continue }
        var e = r; while (e < line.length && line.charAt(e) === "`") e++
        var len = e - r
        ;(runs[len] = runs[len] || []).push(r)
        r = e
    }
    var runUsed = {}
    function flush() { if (text !== "") { out.push({ t: "text", s: text }); text = "" } }
    var i = 0
    while (i < line.length) {
        var c = line.charAt(i)
        if (c === "\\" && i + 1 < line.length && isPunct(line.charAt(i + 1))) {
            text += line.charAt(i + 1); i += 2; continue
        }
        if (c === "`") {
            var n = 0; while (i + n < line.length && line.charAt(i + n) === "`") n++
            var list = runs[n] || [], k = runUsed[n] || 0
            while (k < list.length && list[k] <= i) k++
            runUsed[n] = k
            if (k < list.length) {
                flush()
                var body = line.substring(i + n, list[k])
                if (body.length > 2 && body.charAt(0) === " " && body.charAt(body.length - 1) === " ") body = body.substring(1, body.length - 1)
                out.push({ t: "code", s: body })
                runUsed[n] = k + 1
                i = list[k] + n
            } else { text += line.substring(i, i + n); i += n }
            continue
        }
        if (allowLinks && c === "[") {
            var close = brackets[i + 1]
            if (close > i && line.charAt(close) === "]" && line.charAt(close + 1) === "(") {
                var end = parens[close + 2]
                if (end > close + 2 && line.charAt(end) === ")") {
                    var href = line.substring(close + 2, end)
                    if (urlOk(href)) {
                        flush()
                        var label = line.substring(i + 1, close)
                        out.push({ t: "link", href: href, label: label === "" ? [{ t: "text", s: href }] : tokens(label, false),
                                   bare: label === href })
                        i = end + 1
                        continue
                    }
                }
            }
            text += c; i++; continue
        }
        if (allowLinks && (c === "h" || c === "H") && line.substring(i, i + 8).toLowerCase() === "https://") {
            var u = urlAt(line, i)
            if (urlOk(u)) {
                flush()
                out.push({ t: "link", href: u, label: [{ t: "text", s: u }], bare: true })
                i += u.length
                continue
            }
            // Not a link: the whole run is text, so a long one is read once.
            text += u.length > 8 ? u : line.substring(i, i + 8); i += Math.max(8, u.length); continue
        }
        if (c === "*" || c === "_") {
            var m = 0; while (i + m < line.length && line.charAt(i + m) === c) m++
            var before = i > 0 ? line.charAt(i - 1) : "", after = i + m < line.length ? line.charAt(i + m) : ""
            var left = !isSpace(after) && (!isPunct(after) || isSpace(before) || isPunct(before))
            var right = !isSpace(before) && (!isPunct(before) || isSpace(after) || isPunct(after))
            var open = c === "*" ? left : left && (!right || isPunct(before))
            var shut = c === "*" ? right : right && (!left || isPunct(after))
            flush()
            out.push({ t: "delim", ch: c, n: m, rem: m, open: open, close: shut, opens: [], closes: [] })
            i += m
            continue
        }
        text += c; i++
    }
    flush()
    emphasis(out)
    return out
}

// Pair runs of * and _ into <b> and <i>, nearest opener first. Pairs nest by
// construction: when a closer takes an opener, openers of the other character
// opened since are dropped (they stay text), so no two pairs can cross.
function emphasis(toks) {
    var stacks = { "*": [], "_": [] }
    for (var k = 0; k < toks.length; k++) {
        var d = toks[k]
        if (d.t !== "delim") continue
        d.pos = k
        if (d.close) {
            var same = stacks[d.ch], other = stacks[d.ch === "*" ? "_" : "*"]
            while (d.rem > 0 && same.length > 0) {
                var o = same[same.length - 1]
                while (other.length > 0 && other[other.length - 1].pos > o.pos) other.pop()
                var use = d.rem >= 2 && o.rem >= 2 ? 2 : 1
                var tag = use === 2 ? "b" : "i"
                o.rem -= use; d.rem -= use
                o.opens.push(tag); d.closes.push(tag)
                if (o.rem === 0) same.pop()
            }
        }
        if (d.open && d.rem > 0) stacks[d.ch].push(d)
    }
}

function emit(toks, c, lineStart) {
    var out = []   // joined once: growing one string is quadratic in QML's engine
    for (var k = 0; k < toks.length; k++) {
        var x = toks[k], first = lineStart && out.length === 0
        if (x.t === "text") out.push(keepSpaces(esc(x.s), first))
        else if (x.t === "code") out.push("<font color=\"" + c.code + "\">" + esc(x.s).replace(/ /g, "&nbsp;") + "</font>")
        else if (x.t === "link") {
            out.push("<a href=\"" + esc(x.href) + "\">" + emit(x.label, c, false) + "</a>")
            if (!x.bare) out.push(" <font color=\"" + c.dim + "\">(" + esc(hostOf(x.href)) + ")</font>")
        } else if (x.t === "delim") {
            for (var a = 0; a < x.closes.length; a++) out.push("</" + x.closes[a] + ">")
            if (x.rem > 0) out.push(esc(new Array(x.rem + 1).join(x.ch)))
            for (var b = x.opens.length - 1; b >= 0; b--) out.push("<" + x.opens[b] + ">")
        }
    }
    return out.join("")
}

function inline(line, c) { return emit(tokens(line, true), c, true) }

// The whole body. `colours`: {code, quote, dim} as #rrggbb.
function render(text, colours) {
    colours = colours || {}
    var c = { code: colour(colours.code, "#f5925e"), quote: colour(colours.quote, "#8b93a1"), dim: colour(colours.dim, "#8b93a1") }
    var lines = String(text).split(/\r\n|\r|\n/)
    var out = [], fence = null, code = []
    for (var i = 0; i < lines.length; i++) {
        var line = lines[i]
        if (fence) {
            if (new RegExp("^ {0,3}" + (fence.ch === "`" ? "`" : "~") + "{" + fence.n + ",}\\s*$").test(line)) {
                out.push({ pre: true, html: "<pre>" + esc(code.join("\n")) + "</pre>" })
                fence = null; code = []
            } else code.push(line.replace(/\t/g, "    "))
            continue
        }
        var f = /^ {0,3}(`{3,}|~{3,})/.exec(line)
        if (f) { fence = { ch: f[1].charAt(0), n: f[1].length }; continue }
        var q = /^ {0,3}((?:> ?){1,})(.*)$/.exec(line)
        if (q) {
            var level = Math.min(3, (q[1].match(/>/g) || []).length)
            out.push({ html: "<font color=\"" + c.quote + "\">" + new Array(level + 1).join("▎") + "&nbsp;" + emit(tokens(q[2], true), c, false) + "</font>" })
            continue
        }
        var ul = /^( *)[-*+] +(.*)$/.exec(line)
        if (ul) {
            out.push({ html: new Array(Math.min(4, Math.floor(ul[1].length / 2)) * 3 + 1).join("&nbsp;") + "•&nbsp;" + emit(tokens(ul[2], true), c, false) })
            continue
        }
        var ol = /^( *)(\d{1,9})([.)]) +(.*)$/.exec(line)
        if (ol) {
            out.push({ html: new Array(Math.min(4, Math.floor(ol[1].length / 2)) * 3 + 1).join("&nbsp;") + ol[2] + esc(ol[3]) + "&nbsp;" + emit(tokens(ol[4], true), c, false) })
            continue
        }
        out.push({ html: inline(line, c) })
    }
    if (fence) out.push({ pre: true, html: "<pre>" + esc(code.join("\n")) + "</pre>" })   // never closed: code to the end
    // Lines join with <br>; a <pre> is a block of its own and brings its own breaks.
    var parts = []
    for (var k = 0; k < out.length; k++) {
        if (k > 0 && !out[k].pre && !out[k - 1].pre) parts.push("<br>")
        parts.push(out[k].html)
    }
    return parts.join("")
}

// The tags render() may produce, for checking any output: true when `html`
// holds nothing else, every tag closed in order, every href an https URL.
function onlyOwnTags(html) {
    var re = /<(\/?)(b|i|pre|a|font|br)((?: href="[^"<>]*")|(?: color="#[0-9a-fA-F]{6}"))?>/g
    var stack = [], last = 0, m
    // Between tags: no markup character, and no entity but the ones esc() and
    // keepSpaces() write.
    function plainText(t) { return !/[<>"]/.test(t) && !/&(?!(?:amp|lt|gt|quot|#39|nbsp);)/.test(t) }
    while ((m = re.exec(html)) !== null) {
        if (!plainText(html.substring(last, m.index))) return false
        last = re.lastIndex
        var closing = m[1] === "/", tag = m[2], attr = m[3] || ""
        if (tag === "br") { if (closing || attr) return false; continue }
        if (closing) { if (attr || stack.pop() !== tag) return false; continue }
        if (tag === "a" && !/^ href="https:\/\//i.test(attr)) return false
        if (tag !== "a" && tag !== "font" && attr) return false
        if (tag === "font" && !attr) return false
        stack.push(tag)
    }
    return stack.length === 0 && plainText(html.substring(last))
}
