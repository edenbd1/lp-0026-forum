# Security model

Anyone can publish on the forum's Delivery topic. So the forum trusts nothing it
receives except what a signature proves: a post is stored and shown only once
its Ed25519 signature checks against the key it names, and its id is the hash of
what was signed. Everything else on the topic is **unsigned** (a request for
history, an announcement that a snapshot exists, a bundle of posts) and is
treated as a hint that can be ignored, never as an instruction.

This page follows the external review in
[issue #1](https://github.com/edenbd1/lp-0026-forum/issues/1) (by vpavlin, at
`151af52`). Every point is fixed in 0.2.0 or stated below as a limit, and each
fix has a test named for what it guarantees.

## What peer-supplied data can no longer do

| # | Was possible | Now | Checked by |
|---|---|---|---|
| 1 | A title or alias with `<img src=…>` made every reader fetch a URL, revealing their IP | Every text that comes from other people is rendered as plain text | `scripts/check-no-remote-fetch.py` (a local server logs 0 requests; 4 before the fix) |
| 2 | An unsigned announcement made every reader dial any address (including 127.0.0.1 and the LAN) and download up to 32 MB | An announcement is acted on only as the answer to a history request this node made, while it is open, once; only public addresses are dialled or announced | `an_unsolicited_announcement_makes_nobody_fetch_anything`, `announced_addresses_must_be_public` |
| 3 | One 120-byte request made a node put ~650 KB on the topic, bypassing the rate limit (6.5 MB for ten requests) | A Delivery answer is capped at 256 KB, at most one a minute, and sent through the same token bucket as posts; the post count is a `COUNT(*)` | `a_history_request_cannot_make_a_node_flood_the_topic` |
| 4 | A post dated 2036 was pinned at the top and broke the node's own catch-up | Posts dated more than 10 minutes ahead are refused; "the newest post" ignores any dated after now | `a_post_dated_in_the_future_is_refused` |
| 5 | A snapshot chunk could cut a UTF-8 character, corrupting the post and failing its signature | Chunks are cut on character boundaries | `snapshot_chunks_never_cut_a_character` |
| 6 | A libstorage without `disc-port` refused the config, and the failure looked like "another app started it" | Init is retried without `disc-port`; a real failure is shown as "Storage failed to start: …" | manual (storage_module builds differ) |
| 7 | An unconfirmed post was resent every 2 minutes forever | The wait doubles with each resend, up to an hour; the post is kept | `an_unconfirmed_post_is_resent_less_and_less_often` |
| 8 | Synchronous delivery calls could freeze the backend for 30 s | `send`, `subscribe`, `createNode` and `start` are asynchronous | the e2e run |
| 9 | A lost upload event blocked snapshots until restart | Uploads have a watchdog, like downloads | manual |
| 10 | A 1,000-post import meant 1,000 refreshes and 1,000 commits | Arrivals reach the view at most once per 150 ms (one refresh for an import), reads are saved at most every 2 s, an import is one transaction | `a_large_import_is_one_commit` |
| – | A snapshot could carry announcements and requests | An import accepts posts only | `a_snapshot_carries_posts_and_nothing_else` |
| – | `forum.log` grew without limit | Rotated at 5 MB, one previous file kept | |

## Limits, stated plainly

- **An alias is not private from someone comparing keys.** An alias post is
  signed by your account's key, so anyone can see that two alias posts, or an
  alias and your account, share a key. It changes the name shown, not who can
  be linked. For unlinkability, post anonymously, or rotate the key.
- **Anonymous means unlinkable by keys, not by network.** An anonymous post has
  a key used once, so no two posts can be linked through their keys. It still
  leaves through your own Delivery node, and the peers it connects to directly
  can see which node published it. Network-level anonymity is Logos Delivery's,
  and the forum adds none of its own.
- **Keys are stored unencrypted** in `forum.db`, protected by the operating
  system's file permissions. A retired or deleted key is overwritten in the
  database (`secure_delete`) and flushed out of the write-ahead log, but a copy
  of the file taken earlier still has it.
- **Shared modules.** Basecamp runs one `delivery_module` and one
  `storage_module` for all apps. The forum pins `delivery_module ~0.2.1`, the
  version it is tested with, so it cannot sit next to an app that needs another
  line. Whichever app initialises `storage_module` first sets its settings for
  all; the forum sets only its network, data directory and port, and uses a
  node another app started as it is.
- **History depends on peers being online.** The logos.test store nodes keep no
  archive, so a newcomer gets history only while some peer that has it is
  online.

## Running nodes on one machine

Tests run several Basecamps on one machine, which can reach each other only
through loopback or LAN addresses. `LOGOS_FORUM_LOCAL_PEERS=1` lets such a node
announce and dial those addresses. Never set it on a node that talks to the
public network.
