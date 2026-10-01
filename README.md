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
  <a href="https://github.com/edenbd1/logos-forum-catalog"><img alt="catalog 0.3.2" src="https://img.shields.io/badge/catalog-0.3.2-e2552b"></a>
  <img alt="licence MIT / Apache-2.0" src="https://img.shields.io/badge/licence-MIT%20%2F%20Apache--2.0-7a9a3a">
</p>

[![Watch the demo on YouTube (3:43)](https://img.youtube.com/vi/xNY5EzhCtuI/maxresdefault.jpg)](https://youtu.be/xNY5EzhCtuI)

**▶ [Watch the 3-minute demo](https://youtu.be/xNY5EzhCtuI)**: install, two live nodes, alias, anonymous, key rotation, offline, a newcomer getting the history.

> **Status:** the forum runs in **Logos Basecamp 0.3.0** (macOS and Linux) and is
> installable from its [catalog](#install-in-basecamp). It has been tested end to
> end on five real nodes, 24 checks, on logos.test and again on logos.dev, the
> network it uses by default since testnet v0.3
> ([`docs/e2e.md`](docs/e2e.md)); the core has 47 tests of its own; CI is green
> on Linux and macOS. logos.test with RLN is supported as an opt-in, tested up to
> the membership (modules load, the membership is requested, reading works;
> posting needs a funded membership; see
> [logos.test and RLN](#logostest-and-rln-opt-in)); it is off by default.

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
- **Coming back online catches up, even on a network that keeps nothing.**
  A returning node asks the network's store nodes, and it asks the forum: a
  peer holding more posts sends the most recent ones as bundles over **Logos
  Delivery**, through the relays, so neither side learns the other's address,
  and every post is checked like a live one. On logos.test today the store
  nodes keep no archive at all (measured, see [`docs/e2e.md`](docs/e2e.md)), so
  this is the path that actually works. A node can also opt in to fetching a
  peer's full snapshot from **Logos Storage**, at the cost of connecting to that
  peer. Duplicates are impossible because ids are content.
- **It does not flood the network.** Sends are paced by a token bucket, and
  history answers are capped and paced too.
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
| No centralised server or service | Logos Delivery for posts, Logos Storage for history, SQLite on the device |
| If offline when a message arrived, obtain past messages | store query, then `Engine::request_history` → peers' bundles over Delivery, or (opt-in) a peer's snapshot on Logos Storage (`import_snapshot`); tested on five nodes |
| If a send fails, the text stays locally to retry | the outbox, `Store::failed` + `backoff_ms` |
| Does not flood the network | `RateLimiter` |
| Basecamp app, loadable, in a module catalog | loads in Basecamp 0.3.0; served by [`logos-forum-catalog`](https://github.com/edenbd1/logos-forum-catalog), installed from it on a blank Basecamp |
| Video demo, FURPS self-assessment | [video](https://youtu.be/xNY5EzhCtuI), [solution file](submission/LP-0026.md) |

## Install in Basecamp

**From the catalog** (Basecamp 0.3.0, macOS Apple silicon or Linux x86_64): *Settings → Package Repositories → Add
a repository*, paste
`https://raw.githubusercontent.com/edenbd1/logos-forum-catalog/main/logos-repo.json`,
then *Package Manager → Social → Logos Forum → Install*. Basecamp installs
`delivery_module` and `storage_module` from the official catalog with it.

1. **Add the repository** in *Settings → Package Repositories*:

   ![Adding the Logos Forum repository in Basecamp's settings](docs/screens/01-add-repository.png)

2. **Find the forum** in *Package Manager → Social*:

   ![Logos Forum in Basecamp's package manager](docs/screens/02-package-manager.png)

3. **Install.** Basecamp shows the two modules it brings in from the official catalog:

   ![The install dialog, with storage_module and delivery_module from Logos Official](docs/screens/03-install-with-dependencies.png)

4. **Open it** from the sidebar. An account exists already, and the history
   arrives from peers who are online.

**From source**, on a clean machine (macOS or Linux, [Nix](https://nixos.org) and
[Basecamp 0.3.0](https://github.com/logos-co/logos-basecamp/releases/tag/0.3.0)):

```bash
git clone https://github.com/edenbd1/lp-0026-forum && cd lp-0026-forum
nix build .#lgx-portable                      # result/logos-logos_forum-module.lgx

# the two modules it depends on, from the official Logos catalog
base=https://github.com/logos-co/logos-modules-release/releases/download
curl -LO $base/delivery_module-v0.3.0/delivery_module-0.3.0.lgx
curl -LO $base/storage_module-v3.0.0/storage_module-3.0.0.lgx

# install all three into a Basecamp user directory and open Basecamp on it
scripts/install-local.sh ~/basecamp-forum result/*.lgx delivery_module-0.3.0.lgx storage_module-3.0.0.lgx
LogosBasecamp --user-dir ~/basecamp-forum     # macOS: ~/Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp
```

Then click *Logos Forum* in the sidebar.

## Deployment and addresses

There is nothing to deploy and no program address: the prize puts the
blockchain out of scope, and the forum runs no server. What plays that role:

| | |
|---|---|
| Module catalog | `https://raw.githubusercontent.com/edenbd1/logos-forum-catalog/main/logos-repo.json` |
| Network | Logos Delivery, `logos.dev` preset (cluster 3, no RLN) by default; `logos.test` (cluster 2, RLN) as an opt-in, `"network": "logos.test"` in `settings.json` or `LOGOS_FORUM_PRESET=logos.test` |
| RLN registry (logos.test only) | `logos:testnet:841312e9…c893` (config account `9tZgjoUVHHWuE9D1cgQSXbYu2gm6uN9baTSERtTa9Str`) on the LEZ testnet zone behind `http://209.38.241.182:3240`, as delivery_module 0.3.0's preset names it |
| Forum topic | `/logos-forum/1/logos-forum-934410ad/json`, on shard `/waku/2/rs/2/6` |
| History | peers' bundles over Delivery; peers' snapshots on Logos Storage (same network) when opted in |
| Store nodes queried | the six fleet nodes of the preset (`delivery-01`, `delivery-02` in `do-ams3`, `gc-us-central1-a` and `ac-cn-hongkong-c` for `logos.dev`; `node-01`, `node-02` for `logos.test`) |
| Data on your machine | `<Basecamp user dir>/module_data/logos_forum/` (`forum.db`, `forum.log`) |

A separate forum can be run by starting Basecamp with `LOGOS_FORUM_NAME=<name>`;
the topic is derived from the name.

## logos.test and RLN (opt-in)

The forum runs on `logos.dev` by default, which has no RLN: anyone can post
at once. `logos.test`, the testnet v0.3 network, is supported as an opt-in.
Choose it with `"network": "logos.test"` in
`<Basecamp user dir>/module_data/logos_forum/settings.json` (or
`LOGOS_FORUM_PRESET=logos.test`, which wins), then restart Basecamp; the status
line then names the network.

On logos.test a message goes out only with an RLN rate-limit proof, and a
proof needs an RLN membership in the network's registry. Today the sender's
own delivery node enforces it (it will not publish a message without a proof);
the preset leaves proof validation off on relays. So opting in needs:

1. **The RLN modules.** In Basecamp's Package Manager install *RLN Module*
   (`liblogos_rln_module` 0.10.0, from the Logos catalog; it brings
   `liblogos_lez_rln_module` 4.2.1). They are not dependencies of the forum, so
   logos.dev users never download them (about 270 MB); delivery loads them when
   they are installed. Without them the forum says so.
2. **A membership.** Nothing is registered by hand: the RLN module opens its own
   LEZ wallet on the registry's zone and registers a membership by itself as
   soon as that wallet's account holds the price plus a fee reserve, 182,800,000
   native units at today's base fee (price 1,000,000; the unspent part of the
   reserve stays in the account). Until then the forum shows a banner with that
   account (base58, with a Copy button) and the amount, the status line says
   `no RLN membership yet: posting waits`, and own posts are marked `waiting for
   an RLN membership`. The account has to be funded on the registry's zone
   (sequencer `http://209.38.241.182:3240`), by a transfer from an account
   already funded there or by a deposit from the Logos blockchain into that
   zone's channel. The relaunched LEZ testnet at `testnet.lez.logos.co` is a
   different chain with no RLN registry, so its balances cannot pay for one.

Reading, catching up from store nodes and history need no membership; posts
stay in the outbox and go out once it is active. When the epoch's quota
(10-minute epochs) is spent, held posts are marked and go out in the next one.

![logos.test without a membership yet: the banner, and a post waiting for it](docs/screens/rln-no-membership.jpg)

## Walkthrough

Two real Basecamp nodes on logos.test, Alice on the left and Bob on the right,
with no server between them. Every screenshot comes from the demo film.

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
being away, the forum fetches what you missed from the network and from peers'
snapshots on Logos Storage; *Accounts… → Fetch missed posts* does it on demand,
and *Save snapshot to Logos Storage* publishes yours for others.

**Errors** are shown in one line under the header: a refused post (too long,
no title), a send the network gave up on (it is retried), a storage node that
is not ready.

## Any width, down to a phone

The view adapts instead of clipping. Below 720 px it shows one pane at a time
(the topic list, or the open topic with a way back), the header stacks, the
"Post as" explanation gets its own line, and long titles, keys and URLs wrap.
`scripts/responsive-shots.sh` renders the real view against a stand-in backend
(`tests/qml/Harness.qml`) in nine states at seven widths, from 360 px to 1600 px.

![The forum at 360, 414, 700 and 1280 px](docs/responsive/widths.png)

## Test between two real nodes

```bash
scripts/e2e-two-nodes.sh result/*.lgx delivery_module-0.3.0.lgx storage_module-3.0.0.lgx
```

The scripts run on the forum's default network. With
`LOGOS_FORUM_PRESET=logos.test` (and the two RLN packages after the others),
each posting node first waits for an active RLN membership
([`scripts/e2e-rln.sh`](scripts/e2e-rln.sh)): `E2E_RLN_HOME` keeps the nodes'
memberships between runs, `E2E_FUND` is called with the account and amount to
fund.

Starts two Basecamp instances on logos.test, drives the forum's own interface
and checks each node's store: a topic from A reaches B with the same author
key; B's anonymous reply reaches A under a key none of B's accounts hold; and
B, wiped to a fresh install, recovers both posts from A's snapshot on Logos
Storage. Output of a run: [`docs/e2e/e2e-two-nodes.out`](docs/e2e/e2e-two-nodes.out).

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
