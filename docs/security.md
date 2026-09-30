# Security model

Anyone can publish on the forum's Delivery topic. So the forum trusts nothing it
receives except what a signature proves: a post is stored and shown only once
its Ed25519 signature checks against the key it names, and its id is the hash of
what was signed. Everything else on the topic is **unsigned** (a request for
history, an announcement that a snapshot exists, a bundle of posts). What each
of those can make a node do is bounded below.

This page follows the external review in
[issue #1](https://github.com/edenbd1/lp-0026-forum/issues/1) (by vpavlin, at
`151af52`), and a second, independent pass over the fixes. Each fix has a test
named for what it guarantees.

## How history reaches a newcomer

By default, **over Logos Delivery only**. A node that comes online asks on the
forum's topic; a peer that holds more posts sends them as bundles on that same
topic, newest first. The bundles travel through the relay network, so the asker
never connects to the sender and neither learns the other's address. Every post
in a bundle is checked like a live one. An answer is capped; a full one says so
(`"more"`), and the asker asks again for the page below, starting after the
oldest *verified* post it received (a date and id cursor, so posts sharing one
date do not loop). If several peers answer, the least advanced cursor is used,
so a replayed old post cannot make it skip what lies between. The cursor
survives an answer that never comes: the next periodic request resumes from
it. So a node that was away for long gets everything posted since it left,
even past an answering peer's hourly budget. "Since it left" is the newest post
it held when the session began.

**Logos Storage** snapshots are off by default, in both directions. Fetching
one means connecting to whoever provides it (directly, or found through the
DHT), and offering one means announcing your storage node's address and peer
id. A node that does not use Storage answers Storage-style requests over
Delivery instead, so nobody is left without history. Snapshots can be turned
on with `LOGOS_FORUM_FETCH_SNAPSHOTS=1`, or `"fetchSnapshots": true` in the
forum's `settings.json`, by someone who prefers a one-shot complete archive to
that privacy. Even then, an announcement is followed only as the answer to a
request this node has open, and only public IP addresses are dialled.

## What unsigned traffic can make a node do

| Message | At most | Checked by |
|---|---|---|
| A history request | One answer a minute, 256 KB per answer, 1 MB per hour in total, paced by the same token bucket as posts. With Storage off (the default), a Storage-style request is answered the same way | `a_history_request_cannot_make_a_node_flood_the_topic`, `sustained_requests_cost_a_node_a_bounded_budget`, `a_storage_request_is_answered_over_delivery_by_a_private_node` |
| A history request, Storage opted in | One snapshot upload every ten minutes, and requests in between are not answered | engine rate limit |
| A bundle of posts | Its valid posts are stored, like live ones. It stands the other answerers of the same request down only if it carries everything their own first page would have (which makes it the real answer); an empty, forged or partial replay does not, and neither does an announcement. Its `"more"` marks a next page, whose start is computed from its verified posts, never from its fields | `an_empty_or_forged_bundle_does_not_silence_the_answerers`, `a_replayed_partial_bundle_does_not_silence_the_answerers`, `a_replay_of_the_newest_posts_does_not_silence_the_answerers`, `a_forged_page_marker_cannot_derail_the_paging` |
| An announcement | Nothing, unless snapshot fetching is on and it answers our own open request (then one fetch, public addresses only) | `an_unsolicited_announcement_makes_nobody_fetch_anything`, `announced_addresses_must_be_public` |
| Anything inside a snapshot or bundle other than posts | Nothing | `a_snapshot_carries_posts_and_nothing_else` |

## The review, point by point

| # | Was possible | Now | Checked by |
|---|---|---|---|
| 1 | Rich text in a title or alias made readers fetch a URL (revealing their IP) | Titles, aliases, names, bodies and the error line are plain text; the status line, which can carry a store node's error text, is escaped | `scripts/check-no-remote-fetch.py`: 0 requests (5 on the previous build), with a rich-text control that must be fetched |
| 2 | An unsigned announcement made readers dial any address and download up to 32 MB | See above: no Storage fetch or offer by default; when on, only answers to our open request, once, public IPs only (DNS names refused; addresses parsed, with loopback, private, link-local, CGNAT, documentation, mapped, translated, NAT64, 6to4 and Teredo forms refused) | tests above |
| 3 | One 120-byte request put ~650 KB on the topic, bypassing the rate limit (6.5 MB for ten; 12.3 MB an hour sustained) | Bounded as in the first table: about 1.0 MB for the ten, at most 1 MB an hour sustained; the answer reads only the posts it sends (`ORDER BY ts DESC LIMIT`), counts use `COUNT(*)` | tests above |
| 4 | A post dated 2036 was pinned at the top and broke catch-up | Posts dated more than 10 minutes ahead are refused; `since` ignores posts dated after now + 10 minutes; any stored before this release are hidden from the topic list and never sent on in answers | `a_post_dated_in_the_future_is_refused` |
| 5 | Snapshot chunks could cut a UTF-8 character | Chunks are cut on character boundaries | `snapshot_chunks_never_cut_a_character` |
| 6 | A libstorage without `disc-port` refused the config, reported as "another app started it" | Retried without `disc-port`; init and start failures are reported | manual |
| 7 | An unconfirmed post was resent every 2 minutes forever | The wait doubles with each resend up to an hour; the post is kept (so it is still resent hourly) | `an_unconfirmed_post_is_resent_less_and_less_often` |
| 8 | Synchronous delivery calls could freeze the backend | `send`, `subscribe`, `createNode` and `start` are asynchronous; a confirmation that arrives before the send's own reply is kept 30 s and matched | the e2e run |
| 9 | A lost upload event blocked snapshots until restart | One watchdog per upload, re-armed at every step (init, each chunk, finalize) | manual |
| 10 | A 1,000-post import meant 1,000 refreshes and 1,000 commits | An import is one transaction; arrivals reach the view at most every 150 ms (up to three signals, or one for a large import); read markers are saved at most every 2 s and on quit | `a_large_import_is_one_commit` |
| – | Exceptions in network callbacks could abort the process | The store never throws after opening (a failed statement reads as no rows; a post that cannot be queued is refused with an error); the callbacks that process network input (messages, send results, the pump, snapshot imports) are also wrapped | |
| – | `forum.log` grew without limit | Rotated at 5 MB, one previous file kept | |

## Limits, stated plainly

- **A node's own requests are public.** Anyone on the topic sees that some
  node asked for history and how many posts it holds (not which node: requests
  travel through the relays like posts). With snapshots on, a node that offers
  one announces its storage address and peer id, and a node that fetches one
  connects to the provider.
- **An alias is not private from someone comparing keys.** An alias post is
  signed by your account's key, so anyone can link it to your other posts on
  that key. It changes the name shown, not who can be linked. For
  unlinkability, post anonymously, or rotate the key.
- **Anonymous means unlinkable by keys, not by network.** An anonymous post has
  a key used once. It still leaves through your own Delivery node, and the
  peers it connects to directly can see which node published it.
- **Keys are stored unencrypted** in `forum.db`, protected by the operating
  system's file permissions. A retired or deleted key is overwritten
  (`secure_delete`, the WAL is checkpointed after every rotation or deletion,
  and existing databases are rewritten once on upgrade), but a copy of the file
  taken earlier still has it.
- **The storage peer id is stable per install**, so with snapshots on, a peer
  that sees it twice can tell it is the same install. With them off (the
  default), it is never announced.
- **Shared modules.** Basecamp runs one `delivery_module` and one
  `storage_module` for all apps. The forum pins `delivery_module ~0.2.1`, the
  version it is tested with, so it cannot sit next to an app that needs another
  line. Whichever app initialises `storage_module` first sets its settings for
  all; the forum sets only its network, data directory and port, and uses a
  node another app started as it is.
- **History depends on peers being online.** The logos.test store nodes keep
  no archive. Over Delivery, a large backlog arrives in pages of 256 KB about a
  minute apart, and each answering peer spends at most 1 MB an hour, so a
  backlog of several MB behind a single peer takes hours
  (`a_backlog_larger_than_the_hourly_budget_still_arrives`).
- **A stranger can slow history down, not falsify it.** Requests are unsigned,
  so anyone can spend the peers' hourly answer budgets with cheap requests, or
  keep answering with the same page, and newcomers then wait longer. No post
  can be forged, altered or hidden that way: every post is checked, and nothing
  a stranger sends moves the paging past posts not yet received.

## Known issue (0.2.1)

With `storage_module` 3.0.0 (libstorage v0.5), fetching an opted-in snapshot
currently fails to start (`Failed to start chunk download`) even after the
provider is dialled; the forum then falls back to history over Delivery, so the
history still arrives ([run](e2e/e2e-two-nodes-0.2.1-storage3.out)). Snapshots
are off by default, so the default path is not affected.

## Running nodes on one machine

Tests run several Basecamps on one machine, which can reach each other only
through loopback or LAN addresses. `LOGOS_FORUM_LOCAL_PEERS=1` lets such a node
announce and dial those addresses. Never set it on a node that talks to the
public network.
