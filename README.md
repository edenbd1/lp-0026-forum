<p align="center">
  <img src="assets/icon.png" width="128" alt="Logos Forum icon">
</p>

<h1 align="center">Logos Forum</h1>

<p align="center"><b>A forum with no server, inside Logos Basecamp. Every post signed, every author verified.</b></p>

<p align="center">
A community-built <a href="https://github.com/logos-co/logos-basecamp">Logos Basecamp</a> module for <a href="https://github.com/logos-co/lambda-prize/blob/master/prizes/LP-0026.md">λPrize LP-0026</a>.
Topics and replies travel peer to peer over <b>Logos Messaging</b>, each device keeps the history and shares it with newcomers,
and every post is checked by the app itself on arrival.
</p>

<p align="center">
  <img alt="Logos Basecamp 0.3.0" src="https://img.shields.io/badge/Logos%20Basecamp-0.3.0-2f6b4f">
  <img alt="Logos Messaging (delivery_module) 0.3.0" src="https://img.shields.io/badge/Logos%20Messaging-0.3.0-2f6b4f">
  <img alt="network logos.dev" src="https://img.shields.io/badge/network-logos.dev-2f6b4f">
  <a href="https://github.com/edenbd1/logos-forum-catalog"><img alt="catalog 0.3.6" src="https://img.shields.io/badge/catalog-0.3.6-e2552b"></a>
  <img alt="licence MIT / Apache-2.0" src="https://img.shields.io/badge/licence-MIT%20%2F%20Apache--2.0-7a9a3a">
</p>

[![Watch the demo on YouTube (3:43)](https://img.youtube.com/vi/xNY5EzhCtuI/maxresdefault.jpg)](https://youtu.be/xNY5EzhCtuI)

**▶ [Watch the 3-minute demo](https://youtu.be/xNY5EzhCtuI)**: install, two live nodes, alias, anonymous, key rotation, offline, a newcomer getting the history.

> **Status:** the forum runs in **Logos Basecamp 0.3.0** (macOS and Linux) and is
> installable from its [catalog](#install-in-basecamp). It has been tested end to
> end on five real nodes, 24 checks, on logos.test and again on logos.dev
> ([`docs/e2e.md`](docs/e2e.md)); the core has 134 tests of its own; CI is green
> on Linux and macOS, and builds the Windows x86_64 package (a mingw cross
> build, not yet run in Basecamp on Windows; the official catalog has no Windows
> build of `liblogos_lez_rln_module` or `libp2p_module`, so `logos.test` with RLN
> is macOS and Linux only). It runs on `logos.dev`, which needs no RLN, so anyone
> can post with nothing to set up, and a newcomer gets the whole history from
> the network's store nodes even when nobody else is online
> ([run](docs/e2e/e2e-history-offline-0.3.2.out)). Support for `logos.test`
> with RLN, the membership paid by a sponsor, is built on `main` and opt-in, and
> does not work today: the sponsor's registrations are refused ("Incorrect
> fee"), so a node on logos.test gets no membership and cannot post
> ([logos.test, RLN and the sponsor](#logostest-rln-and-the-sponsor)).

![A thread with replies from an account, an anonymous key and an alias, each marked as verified](docs/screens/04-thread.png)

## What makes it different

- **Authors are verified, not claimed.** Every post is Ed25519-signed and the
  signature is checked against the key the post names before it is shown. A
  post that was edited in transit, or that claims someone else's key, is
  dropped. Post ids are the SHA-256 of the signed bytes, so a post is
  tamper-evident and any copy of it is recognised as the same post.
- **Three ways to sign, chosen per post.**
  - *Identity*: one of your accounts; your posts link to each other.
  - *Alias*: the same account under a name you choose for the post.
  - *Anonymous*: a key made for that one post and wiped immediately, so two
    anonymous posts cannot be linked to each other or to you.
- **Identity rotation.** An account can switch to a fresh key after a number
  of posts or a period of time; nothing links the old key to the new one.
- **Nothing you write is lost.** A post is stored the moment you press Post and
  queued; if the network is down it is retried with back-off until it goes.
- **Coming back online catches up.** A returning node asks the network's
  store nodes, which on logos.dev keep the forum's history (a newcomer gets it
  even with every other node offline), and it asks the forum: a peer holding
  more posts sends the most recent ones as bundles over **Logos Delivery**,
  through the relays, so neither side learns the other's address, and every
  post is checked like a live one. That second path is what works on a network
  whose store nodes keep nothing, as the logos.test ones did when we measured
  them on testnet v0.2 ([`docs/e2e.md`](docs/e2e.md)). A node can also opt in to
  fetching a peer's full snapshot from **Logos Storage**. Duplicates are
  impossible because ids are content.
- **It does not flood the network, and the network cannot flood it.** Sends
  are paced by a token bucket, and history answers are capped and paced too.
  What a node stores from others is budgeted per hour of *arrival*: per author
  key, one shared budget for keys it has never seen (anonymous posts use a
  fresh key each), a total for everything live, and a separate budget for
  history, so a real backlog still catches up. A post's own date plays no part,
  so back-dating it changes nothing ([what this does and does not
  stop](docs/security.md#what-a-node-accepts-from-the-network)).
- **Private forums from an invite.** *Create private forum* gives an invite,
  `logosforum://join/…`, that carries a random 32-byte secret and the forum's
  name. Members' posts are the same signed posts, sealed with
  XChaCha20-Poly1305 before they leave the device, on a topic of their own
  that public forum nodes never subscribe to. History comes the same two ways,
  sealed. The public forum's topic and wire format are unchanged
  ([what is and is not protected](docs/security.md#private-forums)).
- **Mute and hide, for you only.** Mute an author by key (never by name: an
  alias is anyone's to type) or hide a topic or reply. It is kept on this
  device for the selected account, and never sent.
- **Keys sealed with a passphrase, and written down as 24 words.** With a
  passphrase, every account key and every private forum's secret is sealed in
  `forum.db` (Argon2id, then XChaCha20-Poly1305) and unlocked once per
  session; reading and posting anonymously work while locked. An account can
  be shown as 24 words (the BIP-39 English list, with a checksum) and restored
  to the same key on a fresh install. Anonymous keys are never derived from
  an account, so no phrase brings one back
  ([keys at rest](docs/security.md#keys-at-rest-and-backups)).
- **Replies to you, by key.** The *Replies to you* view lists replies to your topics,
  replies after yours and `@` mentions of your key's first 16 hex digits, for
  the selected account only, never matched by a name anyone can type. Muted
  authors and hidden posts do not reach it.
- **Formatting without HTML.** Bodies render a small Markdown subset (bold,
  italic, code, quotes, lists, https links) through the module's own parser,
  which escapes everything the author typed: no tag, image or entity from a
  post ever reaches the view.
- **Images on Logos Storage, checked and cleaned.** A post can carry up to
  four images. Each is decoded under size and pixel caps, rebuilt from its
  pixels and re-encoded, so no GPS, EXIF, XMP, comment or ICC profile leaves
  the machine. The post's signed body commits to the SHA-256 of every image;
  a full image is fetched from Logos Storage only when the reader clicks, and
  shown only if it hashes to the signed value. In a private forum the image is
  sealed with the forum's key before it is stored. Images are not offered for
  anonymous posts, because the poster's own Storage node serves them. A 0.3.x
  peer still verifies such a post and shows a line saying it has images.
- **Reactions and search.** Five signed reactions (👍 ❤️ 🎉 👀 ✅), one state per
  key, post and emoji, limited on arrival before and after the signature.
  Search is local (SQLite FTS5) over verified posts only; what you type is
  never query syntax, and muted authors and hidden posts are left out
  ([images, reactions and search](docs/security.md#images-reactions-and-search)).
- **Unsigned traffic cannot steer it.** Titles and names from other people are
  shown as plain text, what a stranger's request can make a node send is
  capped per hour, nothing makes the forum connect to an address a stranger
  chose, and posts dated in the future are refused. See [`docs/security.md`](docs/security.md), written after
  an [external review](https://github.com/edenbd1/lp-0026-forum/issues/1).

## LP-0026 criteria

| Criterion | Where |
|---|---|
| One or more accounts to post from | `core/include/forum/identity.h`: accounts, selected per post |
| Create topics and reply | `Engine::post_topic`, `Engine::post_reply` |
| Reply by id, by alias, or revealing no id | `Mode::Identity`, `Mode::Alias`, `Mode::Anonymous` |
| Privacy of a long-lived identity; rotation | `RotationPolicy`, `rotate()`; per-post anonymous keys |
| No centralised server or service | Logos Delivery for posts and history (store nodes and peers' bundles), Logos Storage snapshots when opted in, SQLite on the device |
| If offline when a message arrived, obtain past messages | store query, then `Engine::request_history` → peers' bundles over Delivery, or (opt-in) a peer's snapshot on Logos Storage (`import_snapshot`); tested on five nodes |
| If a send fails, the text stays locally to retry | the outbox, `Store::failed` + `backoff_ms` |
| Does not flood the network | `RateLimiter` for what a node sends; `Intake` (`core/include/forum/intake.h`) for what it stores from others, per hour of arrival |
| Basecamp app, loadable, in a module catalog | loads in Basecamp 0.3.0; served by [`logos-forum-catalog`](https://github.com/edenbd1/logos-forum-catalog), installed from it on a blank Basecamp |
| Video demo, FURPS self-assessment | [video](https://youtu.be/xNY5EzhCtuI), [solution file](submission/LP-0026.md) |

## Install in Basecamp

**From the catalog** (Basecamp 0.3.0, macOS Apple silicon, Linux x86_64 or Windows x86_64): *Settings → Package Repositories → Add
a repository*, paste
`https://raw.githubusercontent.com/edenbd1/logos-forum-catalog/main/logos-repo.json`,
then *Package Manager → Social → Logos Forum → Install*. Basecamp installs
what the forum needs with it: `delivery_module` and `storage_module` from the
official catalog. The catalog's forum runs on logos.dev, which needs no RLN:
nothing else to do, no token, no wallet, no account to fund. (The RLN modules
and `rln_gifter_module` that logos.test would need are not in the catalog's
package and are not installed.)

1. **Add the repository** in *Settings → Package Repositories*:

   ![Adding the Logos Forum repository in Basecamp's settings](docs/screens/01-add-repository.png)

2. **Find the forum** in *Package Manager → Social*:

   ![Logos Forum in Basecamp's package manager](docs/screens/02-package-manager.png)

3. **Install.** Basecamp shows the two modules it brings in from the official catalog:

   ![The install dialog, with storage_module and delivery_module from Logos Official](docs/screens/03-install-with-dependencies.png)

4. **Open it** from the sidebar. An account exists already, and the history
   arrives from the store nodes and from peers who are online. You can post at
   once: logos.dev needs no membership.

**From source**, on a clean machine (macOS or Linux, [Nix](https://nixos.org) and
[Basecamp 0.3.0](https://github.com/logos-co/logos-basecamp/releases/tag/0.3.0)).
This builds `main` (0.4.0), which is ahead of the catalog's 0.3.x release
branch; on logos.dev, the default, it needs the same two modules:

```bash
git clone https://github.com/edenbd1/lp-0026-forum && cd lp-0026-forum
nix build .#lgx-portable                      # result/logos-logos_forum-module.lgx

# the modules it depends on, from the official Logos catalog
base=https://github.com/logos-co/logos-modules-release/releases/download
for m in delivery_module-v0.3.0/delivery_module-0.3.0 storage_module-v3.0.0/storage_module-3.0.0; do
  curl -LO $base/$m.lgx; done
# only for logos.test (opt-in, see below): liblogos_rln_module 0.10.0,
# liblogos_lez_rln_module 4.2.1 and libp2p_module 1.1.0 from the same catalog, and
# rln_gifter_module built from logos-rln-gifter c6d854a with gifter/rln-gifter-module.patch

# install them into a Basecamp user directory and open Basecamp on it
scripts/install-local.sh ~/basecamp-forum result/*.lgx *.lgx
LogosBasecamp --user-dir ~/basecamp-forum     # macOS: ~/Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp
```

Then click *Logos Forum* in the sidebar.

## Deployment and addresses

There is nothing to deploy and no program address: the prize puts the
blockchain out of scope, and the forum runs no server. What plays that role:

| | |
|---|---|
| Module catalog | `https://raw.githubusercontent.com/edenbd1/logos-forum-catalog/main/logos-repo.json` |
| Network | Logos Delivery, `logos.dev` preset (cluster 3, no RLN) by default; `logos.test` (cluster 2, RLN) with `"network": "logos.test"` in `settings.json` or `LOGOS_FORUM_PRESET=logos.test` |
| RLN registry (logos.test) | `logos:testnet:841312e9…c893` (config account `9tZgjoUVHHWuE9D1cgQSXbYu2gm6uN9baTSERtTa9Str`) on the LEZ testnet zone behind `http://209.38.241.182:3240`, as delivery_module 0.3.0's preset names it |
| Membership sponsor | `/ip4/88.160.11.28/tcp/24026/p2p/16Uiu2HAm4XsEj65CPBnXZTZbniEE9SEiRtJUmngGuQxQoGFSHxA6`, paying from `2mDx4MEQkt3JE1ZxmM3W18Nnq8pEZBnJJzTbHpYw75wm` on that zone ([`gifter/`](gifter), `scripts/gifter-status.sh`); `"gifter"` in `settings.json` or `LOGOS_FORUM_GIFTER` names another, `"off"` turns it off |
| Forum topic | `/logos-forum/1/logos-forum-934410ad/json`, on shard `/waku/2/rs/3/6` on logos.dev (cluster 3) and `/waku/2/rs/2/6` on logos.test (cluster 2) |
| History | the store nodes and peers' bundles over Delivery; peers' snapshots on Logos Storage when opted in (fetched over Mix by default, a path that has not yet passed end to end: see [`docs/security.md`](docs/security.md#snapshots-over-mix-opt-in)) |
| DNS | the fleet's `/dns4/` addresses are resolved through the name servers the system uses (`scutil --dns` on macOS, `/etc/resolv.conf` on Linux, the adapters on Windows), then 1.1.1.1 and 1.0.0.1, so networks that block public DNS still find peers; `LOGOS_FORUM_DNS=a,b` or `"dns": ["a", "b"]` in `settings.json` sets them by hand; `forum.log` names the ones used |
| Store nodes queried | the six fleet nodes of the preset (`delivery-01`, `delivery-02` in `do-ams3`, `gc-us-central1-a` and `ac-cn-hongkong-c` for `logos.dev`; `node-01`, `node-02` for `logos.test`) |
| Data on your machine | `<Basecamp user dir>/module_data/logos_forum/` (`forum.db`, `forum.log`, `settings.json`), a directory only its owner can open, with `forum.db` readable by its owner only |

A separate forum can be run by starting Basecamp with `LOGOS_FORUM_NAME=<name>`;
the topic is derived from the name.

## logos.test, RLN and the sponsor

The forum runs on `logos.dev` by default. It can also run on `logos.test`, the
testnet v0.3 network (`"network": "logos.test"`, opt-in). There a
message goes out only with an RLN rate-limit proof, and a proof needs an RLN
membership in the network's registry, which costs native balance on the
registry's LEZ zone. Forum users do not need any: the forum's author runs a
**sponsor**, an open RLN gifter (LIP-158,
[`logos-rln-gifter`](https://github.com/logos-co/logos-rln-gifter)), and the
forum asks it for a membership by itself.

- On first launch the RLN module makes this node's RLN identity and keeps its
  secret; the forum sends only its public commitment to the sponsor, which
  registers it on the registry and pays (price 1,000,000 units plus the fee).
  The banner reads *Getting you a membership from the forum's sponsor…*, then
  *Registering the RLN membership*, then goes away once it is active
  (100 messages per 10-minute epoch, for 30 days; the forum asks again after).
- If the sponsor cannot be reached or cannot pay, the banner says why, the
  forum retries after 1, 3, 10 and then every 30 minutes, and *Try again now*
  asks at once.
- Reading, catching up and history never wait for it; posts stay in the
  outbox and go out once the membership is active. When the epoch's quota is
  spent, held posts are marked and go out in the next one.
- **It does not grant memberships today.** Tested from a fresh Basecamp
  through the sponsor and back
  ([`docs/e2e/e2e-sponsor-0.4.0-logos.test-unfunded.out`](docs/e2e/e2e-sponsor-0.4.0-logos.test-unfunded.out)):
  the sponsor's account holds less than the fee reserve a registration needs,
  the zone's sequencer refuses its registration with `Incorrect fee`
  (`wallet_ffi` error 18), and that refusal reaches the forum's banner in 15 s.
  Until the sponsor is funded, a node on logos.test gets no membership and its
  posts stay in the outbox; the protocol path up to the payment works.

  ![The banner when the sponsor cannot pay yet](docs/screens/rln-sponsor-unfunded.jpg)
- What the sponsor sees and cannot see, and why it has no limits:
  [`docs/security.md`](docs/security.md#rln-and-the-forums-sponsor).

`logos.dev`, which runs no RLN, stays selectable: `"network": "logos.dev"` in
`<Basecamp user dir>/module_data/logos_forum/settings.json` (or
`LOGOS_FORUM_PRESET=logos.dev`, which wins), then restart Basecamp. With
`"gifter": "off"` the RLN module registers a membership itself once its own
LEZ account holds the price plus a fee reserve (182,800,000 units at today's
base fee); the banner then shows that account and the amount:

![logos.test without a membership yet: the banner, and a post waiting for it](docs/screens/rln-no-membership.jpg)

### Running the sponsor

The sponsor is a headless Logos runtime (`logosctl` 0.3.1) with
`liblogos_lez_rln_module` (its LEZ wallet pays), `libp2p_module` (a fixed key,
so its peer id never changes) and `rln_gifter_module` serving open, kept up on
a Mac by the LaunchAgent `co.logos.forum-gifter`, which also maps its port on
the router by UPnP ([`gifter/`](gifter)):

```bash
gifter/install.sh rln_gifter_module.lgx       # into ~/logos-forum-gifter, port 24026
scripts/gifter-status.sh                      # agent, port, mapping, address, payer, balance
```

Each membership costs the sponsor the price (1,000,000) and the fee (74M to
83M at base fee 8, as measured upstream on devnet; not yet measured on
logos.test), and its account must hold a fee reserve of about 181.8M at
the moment it registers, so it is funded with native units on the registry's
zone (`http://209.38.241.182:3240`), for instance by a deposit from the Logos
blockchain into that zone's channel.

## Walkthrough

Two real Basecamp nodes, Alice on the left and Bob on the right, with no
server between them. The screenshots come from the demo film, recorded with
version 0.1 on logos.test before the forum moved to logos.dev; the flow is the
same today, and the RLN banner is new.

**A topic goes out and arrives, already verified.** Alice asks a question; a
moment later Bob has it, with a ✓ for the signature his own app checked.

![Alice posts a topic and it shows up on Bob's side](docs/screens/05-two-nodes-live.png)

**Under an alias.** Bob answers as "Ghost". The name is his choice for this
post; the post is still signed by his key, and marked **alias** for everyone.

<p><img src="docs/screens/06-alias-compose.png" width="49%" alt="Bob picks Alias and types Ghost"> <img src="docs/screens/07-alias-received.png" width="49%" alt="The alias reply on both sides"></p>

**Anonymously.** Alice picks *Anonymous*: the app makes a key for that one
post, signs with it and wipes it. The explanation under the picker says what
the others will see.

![Alice replies anonymously](docs/screens/08-anonymous-compose.png)

**Key rotation.** *Accounts… → Rotate now* gives the account a brand new key.
On Bob's side, Alice's posts before and after show two unrelated keys.

<p><img src="docs/screens/09-rotation.png" width="49%" alt="The Accounts dialog with Rotate now"> <img src="docs/screens/10-rotation-seen-by-others.png" width="49%" alt="Bob sees two unrelated keys"></p>

**Offline.** Bob has no network. His reply is saved on his machine, marked
*sending…*, and the header counts *1 waiting to send*. When he is back online
it goes out on its own and Alice gets it.

<p><img src="docs/screens/11-offline-waiting.png" width="49%" alt="A reply waiting for the network"> <img src="docs/screens/12-offline-delivered.png" width="49%" alt="The reply delivered once Bob is back"></p>

**A newcomer gets the history.** A brand new install asks the peers already
there and receives every topic and every reply, each checked again on arrival
(*History from peers · 13 new*).

<img src="docs/screens/13-newcomer-history.png" width="60%" alt="A fresh install with the whole history from peers">

## Using the forum

**Your first minute.** Open *Logos Forum* in Basecamp's sidebar. An account
("Account 1") is created on first launch, so you can post straight away. The
header shows the network state (*Connected*, or *Joined, waiting for peers*)
and, under it, the history state: when the forum last caught up, and whether
Logos Storage is ready.

**Read.** The left column lists topics, most recently active first, with a
line of their text, their author and reply count; an orange dot marks activity
since you last opened a topic, and the search box filters by title, text or
author. Click one to open it; replies follow in order.
Every author line carries a ✓ and the first bytes of the key the post's
signature was checked against; hover it for the full key. A post whose
signature does not check is never shown.

**Post.** *New topic* opens the composer: a title, a body, and *Post as*.
Reply from the box under an open topic.

| Post as | What others see | What it links |
|---|---|---|
| *your account* | the account's key (or its label, on your own machine) | all posts by that account |
| *alias* | the name you type, marked **alias** | posts by the same account, to anyone comparing keys |
| *anonymous* | "anonymous", marked **anonymous**, under a key made for that post and wiped | nothing: two anonymous posts share no key |

**Accounts** (*Accounts…*): create several and switch between them; the one
selected signs what you post next. **Identity rotation** replaces the selected
account's key with a fresh one, now or automatically after a number of posts
or days. Nothing links the old key to the new one.

**Offline.** Write as usual: the post appears at once, marked *sending…*,
then *waiting for the network, will retry*, and the header counts what is
waiting. A post leaves that state only when the network confirms it has it:
Logos Delivery accepts a send even with no peers and retries it only in
memory, so the forum keeps the post itself and sends it again as soon as the
connection returns, or after two minutes without confirmation, or with
back-off (2 s … 5 min) after an error. This holds even after Basecamp was closed.
When you come back after
being away, the forum fetches what you missed from the network's store nodes
and from peers, over Logos Delivery; *Accounts… → Fetch missed posts* does it
on demand. Snapshots on Logos Storage are off by default (`"fetchSnapshots":
true` in `settings.json` turns them on); *Save snapshot to Logos Storage*
publishes yours for others.

**Private forums.** The switcher under the header lists *Logos Forum* and
the private forums you joined (on a phone, *Forums…* holds the actions).
*Create private forum* asks for a name and shows the invite; *Copy invite*
shows it again for the forum you are in; *Join with invite* takes one pasted
from someone else; *Leave* deletes the forum's invite and posts from this
device. Inside, everything works as in the public forum: accounts, aliases,
anonymous posts, the outbox, history. Whoever holds the invite can read and
post, and nobody can be removed: to drop someone, create a new forum and
invite the others again.

![A private forum, with the switcher under the header](docs/screens/14-private-forum.png)

<p><img src="docs/screens/15-private-forum-invite.png" width="49%" alt="The invite of a private forum"> <img src="docs/screens/16-muted-and-hidden.png" width="49%" alt="Muted keys and hidden posts"></p>

**Mute and hide.** Next to someone else's post, *hide* hides it (a topic with
its thread) and *mute author* hides every post signed by that key, in every
forum. On an anonymous post the key signs that post only, so it reads *mute
this key*. *Muted…* lists both, with *Unmute* and *Show again*. This is per
account and stays on this device: it is never sent, and the posts are still
kept and passed on to others.

**Errors** are shown in one line under the header: a refused post (too long,
no title), a send the network gave up on (it is retried), a storage node that
is not ready.

## Any width, down to a phone

The view adapts instead of clipping. Below 720 px it shows one pane at a time
(the topic list, or the open topic with a way back), the header stacks, the
"Post as" explanation gets its own line, and long titles, keys and URLs wrap.
`scripts/responsive-shots.sh` renders the real view against a stand-in backend
(`tests/qml/Harness.qml`) in fifteen states at seven widths, from 360 px to
1600 px, private forums and their dialogs included.

![The forum at 360, 414, 700 and 1280 px](docs/responsive/widths.png)

## Test between two real nodes

```bash
scripts/e2e-two-nodes.sh result/*.lgx delivery_module-0.3.0.lgx storage_module-3.0.0.lgx
```

The scripts run on the forum's default network, logos.dev, with no RLN.
`LOGOS_FORUM_PRESET=logos.test` runs them on logos.test: pass the RLN
packages, `libp2p_module` and `rln_gifter_module` after the others, and each
posting node first waits for a membership from the sponsor
([`scripts/e2e-rln.sh`](scripts/e2e-rln.sh)), which it does not get while the
sponsor cannot pay (see above).

Starts two Basecamp instances, drives the forum's own interface and checks
each node's store: a topic from A reaches B with the same author key; B's
anonymous reply reaches A under a key none of B's accounts hold; and B, wiped
to a fresh install, recovers both posts from A's snapshot on Logos Storage
(this script turns snapshots on for both nodes; they are off by default).
Output of its last recorded run, from version 0.1 on logos.test with
`delivery_module` 0.2.1 and `storage_module` 2.1.2:
[`docs/e2e/e2e-two-nodes.out`](docs/e2e/e2e-two-nodes.out). The current runs are
the five-node scenario on logos.dev
([`e2e-full.run7-0.3.2-logos.dev.out`](docs/e2e/e2e-full.run7-0.3.2-logos.dev.out),
24/24) and history with everyone offline
([`e2e-history-offline-0.3.5.out`](docs/e2e/e2e-history-offline-0.3.5.out)).

## Build and test the core

Needs a C++20 compiler, CMake, libsodium, SQLite and nlohmann-json
(`brew install libsodium sqlite nlohmann-json` on macOS).

```bash
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/forum_core_tests
```

## Credits

The module wiring follows the reference
[`jzaki/forum-sample-app`](https://github.com/jzaki/forum-sample-app) that the
prize lists as a resource. The forum core here is written from scratch.

## Licence

MIT OR Apache-2.0.
