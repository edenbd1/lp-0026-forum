![Logos Forum: a forum with no server, inside Logos Basecamp](docs/screens/banner.png)

# Logos Forum (LP-0026)

A forum for Logos Basecamp with no server anywhere: topics and replies travel
over **Logos Messaging** (Logos Delivery), history is kept locally and on
**Logos Storage**, and every post is signed and checked on arrival. You post as
yourself, under an alias, or as no one at all.

[![Watch the demo on YouTube (3:43)](https://img.youtube.com/vi/xNY5EzhCtuI/maxresdefault.jpg)](https://youtu.be/xNY5EzhCtuI)

**▶ [Watch the 3-minute demo](https://youtu.be/xNY5EzhCtuI)**: install, two live nodes, alias, anonymous, key rotation, offline, a newcomer getting the history.

> **Status:** the forum runs in **Logos Basecamp 0.3.0** (macOS and Linux) and is
> installable from its [catalog](#install-in-basecamp). It has been tested end to
> end on five real nodes on the logos.test network, 24 checks
> ([`docs/e2e.md`](docs/e2e.md)); the core has 47 tests of its own; CI is green
> on Linux and macOS.

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
curl -LO $base/delivery_module-v0.2.1/delivery_module-0.2.1.lgx
curl -LO $base/storage_module-v2.1.2/storage_module-2.1.2.lgx

# install all three into a Basecamp user directory and open Basecamp on it
scripts/install-local.sh ~/basecamp-forum result/*.lgx delivery_module-0.2.1.lgx storage_module-2.1.2.lgx
LogosBasecamp --user-dir ~/basecamp-forum     # macOS: ~/Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp
```

Then click *Logos Forum* in the sidebar.

## Deployment and addresses

There is nothing to deploy and no program address: the prize puts the
blockchain out of scope, and the forum runs no server. What plays that role:

| | |
|---|---|
| Module catalog | `https://raw.githubusercontent.com/edenbd1/logos-forum-catalog/main/logos-repo.json` |
| Network | Logos Delivery, `logos.test` preset (cluster 2) |
| Forum topic | `/logos-forum/1/logos-forum-934410ad/json`, on shard `/waku/2/rs/2/6` |
| History | peers' bundles over Delivery; peers' snapshots on Logos Storage (`logos.test`) when opted in |
| Store nodes queried | the four `logos.test` fleet nodes (`node-01.do-ams3`, `node-01.gc-us-central1-a`, `node-01.ac-cn-hongkong-c`, `node-02.do-ams3`) |
| Data on your machine | `<Basecamp user dir>/module_data/logos_forum/` (`forum.db`, `forum.log`) |

A separate forum can be run by starting Basecamp with `LOGOS_FORUM_NAME=<name>`;
the topic is derived from the name.

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
scripts/e2e-two-nodes.sh result/*.lgx delivery_module-0.2.1.lgx storage_module-2.1.2.lgx
```

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
