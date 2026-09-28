# Video script — LP-0026 Logos Forum (~6 min)

Read in your own voice. **[SCREEN]** says what is on screen; the rest is what
you say. Speak normally — pauses and small mistakes are fine, they are what
makes it a human walk-through. Record screen and voice together (QuickTime →
New Screen Recording, microphone on), one take per part, then join them.

Setup before recording: two Basecamp windows side by side —
`LogosBasecamp --user-dir ~/forum-a` (left, "Node A") and
`--user-dir ~/forum-b` (right, "Node B"), both with nothing installed yet.

---

## 1. What it is — 30 s

**[SCREEN]** The repo README, top.

"This is Logos Forum, my submission for LP-0026. It's a forum for Logos
Basecamp with no server anywhere. Posts travel over Logos Delivery, history
is shared through Logos Storage, and every post is signed and verified when it
arrives — so the name next to a post is a fact, not a claim. You can post as
yourself, under an alias, or completely anonymously."

## 2. Install from the catalog — 45 s

**[SCREEN]** Node A: Settings → Package Repositories. Paste the URL, Add.

"Installation is from a module catalog. I add my repository to Basecamp…"

**[SCREEN]** Package Manager → Social → Logos Forum → Install. Show the dialog.

"…and install Logos Forum. Basecamp pulls its two dependencies, the delivery
and storage modules, from the official Logos catalog. One thing I found here:
unpinned, Basecamp picks storage 3.0 release candidate, which changed its
upload and download calls — so the package pins the 2.1 series."

**[SCREEN]** Do the same on Node B (can be sped up in editing). Open the forum on both.

## 3. Post and receive — 1 min

**[SCREEN]** Node A: New topic → title and body → Post as Account 1 → Post.

"On node A I create a topic. It's stored locally first, then sent."

**[SCREEN]** Node B: the topic appears. Open it. Hover the ✓ key.

"About a second later it's on node B. This check mark and key: node B
verified the Ed25519 signature against this key before showing the post. If
anyone edits a post on the way, or claims someone else's key, it's dropped.
And post ids are the hash of the signed content, so the same post from two
sources is stored once."

## 4. Alias, anonymous, accounts, rotation — 1 min 15

**[SCREEN]** Node B: reply, Post as → alias, type a name, Reply.

"Replies can be signed three ways. Under an alias — the name is mine to
choose, but it's still signed by my account, and the forum says so."

**[SCREEN]** Node B: reply, Post as → anonymous, Reply. Node A: both replies, badges, different keys.

"Or anonymously: the forum makes a key for this one post and wipes it right
after. Look at node A — the anonymous reply has a key that belongs to none of
my accounts, and two anonymous posts never share one."

**[SCREEN]** Node B: Accounts… → Create → switch. Then Rotate now; set "after 20 posts".

"I can hold several accounts and switch between them. And because keeping one
identity forever is itself a privacy cost, an account can rotate to a fresh
key — now, or automatically after a number of posts or days. Nothing links the
old key to the new one."

## 5. Offline, and history — 1 min 30

**[SCREEN]** Turn off Wi-Fi. Node A: post a reply. Show "waiting for the network — will retry" and the waiting counter.

"Now I go offline and keep writing. The post is kept on disk, marked as not
sent yet, and retried with back-off."

**[SCREEN]** Wi-Fi on. The mark disappears; the reply shows on node B.

"When the network comes back, it goes out by itself — even across a restart."

**[SCREEN]** `docs/e2e.md`, the table of four store nodes returning 0.

"Catching up after being away was the interesting part. The standard way is
to ask the network's store nodes. I measured them: all four logos.test store
nodes answer, but keep no messages at all. So a newcomer would see an empty
forum."

**[SCREEN]** Quit node B, delete `~/forum-b/module_data/logos_forum/forum.db`, reopen. Topics come back. Then show its log lines ("dialing snapshot provider…", "snapshot imported").

"So the forum also asks its peers. Node B starts fresh, with nothing. It asks;
node A answers by saving a snapshot to Logos Storage and announcing where it
is. Node B dials A's storage node directly, downloads it, and checks every post
in it like a live one — a snapshot has no authority of its own. Everything is
back."

## 6. Design and tests — 1 min

**[SCREEN]** Repo tree: `core/`, `src/`, then `core/tests/tests.cpp` test names.

"The code is two layers. The core is a plain C++ library with every rule of
the forum — signing, storing, the outbox, pacing, history — behind a small
transport interface. The Basecamp module is a thin layer on top. That's what
let me test all the reliability behaviour without a node: twenty-five tests,
each named for what it guarantees."

**[SCREEN]** Terminal running `scripts/e2e-two-nodes.sh` output ending in PASS. Then the green CI page.

"And this script runs two real Basecamp nodes on the public test network,
drives the forum through its interface, and checks each node's database. CI
builds and tests it on Linux and macOS. It doesn't flood the network either:
sends go through a token bucket."

## 7. Close — 15 s

"No server, authors you can verify, three ways to sign, rotation, offline
posting, and history even on a network that keeps none. Thanks for watching."
