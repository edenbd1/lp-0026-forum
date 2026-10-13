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
| Any history request, before deciding to answer | At most one post count a second, reused for every request in between (a new post resets it). 5,000 requests in a second cost one count, in the engine and in the plugin, which learns what a message added from the engine instead of counting the store | `a_flood_of_unsigned_wants_costs_one_count_per_window`, `what_a_message_added_is_known_without_counting_the_store` |
| A request for a range that holds nothing, or a narrow one | One request waits at a time; a wider one (from the top, then reaching further back) takes the place of a narrower one. An answer that sends nothing starts no cooldown, and a Delivery request that arrives during the cooldown is kept and answered when it ends. So no request a stranger makes can make a node drop a newcomer's | `a_request_for_nothing_does_not_silence_a_node`, `unsigned_requests_cannot_keep_a_newcomer_from_being_answered`, `a_request_during_the_cooldown_is_answered_when_it_ends` |
| A history request, Storage opted in | One snapshot upload every ten minutes, and requests in between are not answered | engine rate limit |
| A bundle of posts | Its valid posts are stored within the intake budgets (below): the history budget only if it answers an open request of this node's, the live budgets otherwise. Comparing it with a pending answer costs about what reading it costs (`checking_a_bundle_against_a_pending_answer_costs_linear_work`). It stands the other answerers of the same request down only if it carries everything their own first page would have (which makes it the real answer); an empty, forged or partial replay does not, and neither does an announcement. Its `"more"` marks a next page, whose start is computed from its verified posts, never from its fields | `an_empty_or_forged_bundle_does_not_silence_the_answerers`, `a_replayed_partial_bundle_does_not_silence_the_answerers`, `a_replay_of_the_newest_posts_does_not_silence_the_answerers`, `a_forged_page_marker_cannot_derail_the_paging` |
| An announcement | Nothing, unless snapshot fetching is on and it answers our own open request (then one fetch, public addresses only) | `an_unsolicited_announcement_makes_nobody_fetch_anything`, `announced_addresses_must_be_public` |
| Anything inside a snapshot or bundle other than posts | Nothing | `a_snapshot_carries_posts_and_nothing_else` |

## What a node accepts from the network

Every post that arrives is parsed and its signature checked; that much is the
cost of listening, bounded by the network's 150 KiB message size (a payload
over 160 KiB is dropped before it is parsed:
`an_oversized_payload_is_dropped_before_it_is_parsed`), and titles over 200
bytes, aliases over 40 and bodies over 20,000 are refused before any budget
(`titles_aliases_and_bodies_over_their_caps_are_refused_on_receipt`). What
follows decides whether a valid, new post is **stored**, which is what costs
disk, a row in the topic list and a place in this node's history answers.

Every window is a sliding hour of **arrival time**: this node's clock when the
post reached it. The date inside a post is its author's choice and is never
used, so dating posts a year back, in reverse order or an hour apart changes
nothing; a clock that steps back does not reopen a window either. A copy of a
post already stored costs nothing, and a refused post is not remembered, so it
can be stored later (from history, when there is room).

| Budget, per hour of arrival | Default | Checked by |
|---|---|---|
| Topics per author key (live) | 20 | `an_author_key_gets_a_bounded_number_of_topics_an_hour` |
| Replies per author key (live), counted apart from topics | 120 | `topics_and_replies_are_counted_apart_per_key` |
| Posts from keys this node holds no post from, all together (live) | 60 | `brand_new_keys_share_one_budget` |
| Everything live, whatever the number of keys | 1,000 | `everything_live_has_an_hourly_total_whatever_the_number_of_keys` |
| History: store nodes' archive, answers to our own open request, a snapshot | 5,000 posts and 16 MiB | `history_is_bounded_too`, `history_has_its_own_budget_so_a_backlog_still_arrives` |
| Dates chosen by the author, a clock stepping back | no effect | `back_dated_or_reordered_posts_cannot_escape_the_budget`, `a_clock_stepping_back_does_not_widen_the_budget` |
| A copy of a stored post | free | `a_copy_of_a_stored_post_costs_no_budget` |
| A bundle that answers no open request of ours | charged as live posts | `a_bundle_nobody_asked_for_is_charged_like_live_posts` |

Where a post came from is decided by this node, never by the message: Logos
Delivery marks what its store catch-up recovered (`source: "history"`), the
forum's own store queries are history, and a bundle is history only if its
`re` names one of this node's requests that is still open.

**History catches up a real backlog.** The history budget is separate, so a
flood that spent the live budgets does not stop a returning node from loading
1,500 posts of one author from the store nodes. Pages a peer sends are cut by
the budget like anything else, and the paging does not skip what was refused:
the next page starts above the first refused post (newest first, ties by id),
the request waits while there is no room, and resumes when there is
(`a_page_cut_short_by_the_budget_resumes_above_its_first_refused_post`,
`posts_the_history_budget_refused_are_not_paged_past`).

**What the shared budget for new keys does, and what it does not.** A per-key
limit alone stops nobody: an anonymous post is signed by a key made for it,
and anyone can make a key per post. The shared budget is the part that holds:
all posts from keys this node has never stored a post from share 60 an hour,
however many keys there are. So a stranger with fresh keys gets at most 60
posts an hour into this node's store and topic list, and every anonymous
post, honest ones included, competes for the same 60. It does not stop a
patient attacker: a key with one stored post is "known", and gets its own 20
topics and 120 replies; making keys known costs one slot each of the 60. The
live total caps the sum at 1,000 posts an hour whatever the number of keys,
and the history budget at 5,000 more, so the store grows by a bounded amount
an hour. Under such a flood, honest posts can be refused live once a budget
is spent; they are not lost to the network, and come back through the store
nodes' archive, under the history budget, at the next catch-up.

The budgets live in memory: a restart opens fresh windows. That is the
user's own action, not something a peer can cause.

## Private forums

A private forum is a random 32-byte secret and a name, carried by its invite
(`logosforum://join/<base64url(1 | secret | name)>`). From the secret,
libsodium's `crypto_kdf` (BLAKE2b, context `LFgroup1`) derives three separate
subkeys: one names the forum's Delivery topic
(`/logos-forum/1/g-<hex>/sealed`), one is the sealing key, one the forum id
signed into its posts (`g:<hex>`). Inside, a private forum is an ordinary
forum: the same signed posts, history requests and bundles, run by the same
engine. Every message is sealed before it reaches Delivery: XChaCha20-Poly1305
with a fresh random nonce, the topic bound in as associated data, the
plaintext padded to a multiple of 1 KiB. The public forum's topic and wire
format do not change, and public forum nodes never subscribe to a private
forum's topic (it shares the public forum's shard, so its traffic mixes with
the forum's on the relays). History is the store nodes' archive of the topic
(sealed, opened on arrival) and members' bundles (sealed). No Storage
snapshots are made of a private forum.

| Guarantee | Checked by |
|---|---|
| A sealed message opens to exactly what was sealed | `a_sealed_message_round_trips` |
| Another group's key cannot open it, and it does not open on another topic | `a_wrong_key_cannot_read_a_sealed_message` |
| A changed byte, a truncation or junk is refused | `a_tampered_sealed_message_is_rejected` |
| The public topic never carries a group post, though both forums share one store and one outbox | `the_public_topic_never_carries_a_group_post` |
| On the wire, a group's posts (in every mode), history requests and bundles carry no title, body, alias, forum name, forum id, author key or message tag, and nothing parses as JSON; a public-only node stores nothing | `nothing_of_a_group_is_in_plaintext_on_the_wire` |
| Sizes on the wire are padded | `sealed_sizes_say_little_about_the_post` |
| A member catches up from the store nodes and from members; an outsider reads nothing | `a_member_catches_up_from_store_nodes_and_from_members_and_an_outsider_reads_nothing` |
| Topic, key and forum id are independent subkeys; invites parse strictly | `the_topic_the_key_and_the_forum_id_are_separate_subkeys`, `an_invite_carries_the_secret_and_the_name_and_nothing_else_parses` |
| Leaving deletes the secret and the forum's posts from the device | `leaving_a_group_deletes_its_secret_and_posts_from_the_device` |

**What is protected:** the content of a private forum (titles, bodies,
aliases, author keys, the forum's name) and who its members are, from anyone
who does not hold the invite, the relays and store nodes included.

**What is not:**

- **Members.** Anyone holding the invite reads everything and can post; a
  member can pass on posts, or the invite itself.
- **No removal.** The secret cannot be revoked. To drop someone, create a new
  forum and invite the others again.
- **Delivery metadata.** The network sees that sealed messages go to a given
  group topic, when, through which relay peers, and their padded size, so it
  can count a group's traffic and link messages of one group to each other.
  The topic name says it is a Logos Forum group, not which one.
- **On disk,** a private forum's posts are stored in `forum.db` like the
  public forum's, unencrypted, protected by the file's `0600` permissions. Its
  secret is kept like the account keys: sealed under the passphrase when there
  is one ([below](#keys-at-rest-and-backups)), otherwise owner-only. Locked, a
  sealed forum is not opened and none can be joined. Once unlocked, a joined
  forum's secret stays in memory until the forum closes: locking again does
  not close forums already open.
- **The intake budgets** apply inside a private forum too, per forum.

## Muting and hiding

A reader can mute an author by **key** (never by display name: an alias is
anyone's to type) and hide a topic or a reply by id. It is stored in
`forum.db` for the selected account, deleted with that account, and never
sent: muting has no network access at all, and what the node stores and
serves to others is unchanged (`muting_and_hiding_are_never_sent_or_shared`).
An anonymous post's key signs that one post, so muting it hides only it.
Tests: `muting_is_by_key_so_a_shared_alias_hides_only_the_muted_author`,
`hiding_a_topic_or_a_reply_hides_only_it`,
`the_muted_list_shows_keys_and_unmute_brings_posts_back`,
`moderation_is_per_account_and_kept_across_a_restart`.

## Keys at rest and backups

**Without a passphrase**, keys are kept as before, in `forum.db`, readable by
its owner only (`0600` in a `0700` directory, its log and shared memory too).

**With a passphrase** (at least 8 characters), every account's secret key is
sealed with XChaCha20-Poly1305 under a key derived by Argon2id
(`crypto_pwhash`, libsodium's MODERATE limits: about a second, once per
session). Each sealed key is bound to its account's public key, and each
private forum's secret to that forum's id, as associated data, so a blob moved
to another row does not open. A sealed check value tells a wrong passphrase
from a damaged file. Setting a passphrase seals the plain keys and rewrites
the file (`secure_delete`, `VACUUM`, WAL checkpoint) so the plain copies leave
`forum.db` and its log; a copy of the file taken earlier still has them.
Reading and posting anonymously work while locked; posting as an account
needs the keys unlocked. Secret keys are wiped from memory when they go away.

**A backup** is the account's 32-byte Ed25519 seed written as 24 words of the
BIP-39 English list with an 8-bit checksum: a mistyped or swapped word is
caught (all but 1 in 256), and restoring gives the same public key. The words
are only an encoding: the seed is not stretched into a wallet seed, so they
are not a wallet's. A phrase restores one account's current key. Anonymous
keys are random and wiped after their one post, so no phrase restores them;
a rotation draws a fresh key and marks the account as needing a new backup.

**Replies to you** are matched by the selected account's key, never by a name:
a reply to a topic that key started, a reply after one it wrote, or `@`
followed by at least the first 16 hex digits of the key. Another account on
the same install has its own view. Replies to anonymous posts do not reach
the inbox, because nothing on disk records which anonymous posts this install
wrote. Muted keys and hidden posts never reach it.

| Guarantee | Checked by |
|---|---|
| Without a passphrase the store, its log and shared memory are owner-only | `without_a_passphrase_the_store_its_log_and_its_shared_memory_are_owner_only` |
| A passphrase seals existing plain keys and leaves no plain copy in the file or its log | `a_passphrase_seals_existing_plain_keys_and_leaves_no_plain_copy_on_disk` |
| A wrong passphrase fails cleanly and changes nothing | `a_wrong_passphrase_fails_cleanly_and_changes_nothing` |
| No plain key can be written next to sealed ones | `a_plain_key_cannot_be_written_next_to_sealed_ones` |
| A sealed key opens only for its own account | `a_sealed_key_opens_only_for_its_own_account` |
| The passphrase can be changed and removed without losing a key | `the_passphrase_can_be_changed_and_removed_without_losing_a_key` |
| Argon2id at desktop strength; a damaged setting is refused | `unlocking_costs_argon2id_at_desktop_strength_and_a_damaged_setting_is_refused` |
| Private forums' secrets are sealed too, and bound to their forum | `a_passphrase_seals_every_private_forums_secret_too`, `a_sealed_private_forum_secret_cannot_be_moved_to_another_forum`, `a_plain_private_forum_secret_moved_to_another_row_is_not_opened_as_that_forum` |
| Secret keys are wiped when they go away | `secret_keys_are_wiped_when_they_go_away` |
| The words are the BIP-39 English list; a restore gives the same key; mistakes are caught | `recovery_words_are_the_bip39_english_list`, `a_restored_account_has_the_same_public_key_on_a_fresh_install`, `a_mistyped_missing_or_swapped_word_is_caught` |
| Anonymous keys are never derived from an account nor restored | `restore_never_reuses_anonymous_keys_and_they_are_never_derived_from_the_account` |
| A rotation marks the account as not backed up | `a_rotation_marks_the_account_as_not_backed_up_and_the_old_words_give_the_old_key` |
| Nothing on disk links a sent anonymous post to this install | `nothing_on_disk_links_an_anonymous_post_to_this_install_once_it_is_sent` |
| Replies and mentions reach an account by key, never by name, and only that account | `replies_and_mentions_reach_you_by_key_never_by_name`, `anonymous_posts_are_not_yours_in_the_inbox`, `a_second_account_sees_neither_the_first_accounts_posts_nor_its_replies` |
| Muted keys and hidden posts never reach the inbox | `muted_keys_and_hidden_posts_never_reach_the_inbox` |

**What it does not protect:** a passphrase protects a copy of `forum.db`
taken while the forum is closed or locked, not a machine where malware runs
while the keys are unlocked. The posts themselves (public, or a private
forum's) are not encrypted on disk.

## Formatting

Post bodies render a Markdown subset (`src/qml/Markdown.js`): bold, italic,
code, fenced code, quotes, lists and https links. Every character the author
typed is escaped on output; the only tags are the parser's own (`<b> <i> <br>
<pre> <a> <font>`), nested in order. There is no image syntax, and only https
URLs with a host become links. The harness's `markdown` mode runs adversarial
cases, and `scripts/check-no-remote-fetch.py` renders a body written to load
something and checks that nothing is fetched without a click.

## The review, point by point

| # | Was possible | Now | Checked by |
|---|---|---|---|
| 1 | Rich text in a title or alias made readers fetch a URL (revealing their IP) | Titles, aliases, names, bodies and the error line are plain text; the status line, which can carry a store node's error text, is escaped. Since 0.3.4, https links in a topic's title and in post bodies can be clicked: see [Links in posts](#links-in-posts) | `scripts/check-no-remote-fetch.py`: 0 requests (5 on the previous build), with a rich-text and a StyledText control that must be fetched |
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

## A second audit: what a peer or a reader of the disk could find

Each was reproduced first (a probe against the core library, or the test
named), then fixed, and each test was checked to fail with the fix undone.

| # | Was possible | Now | Checked by |
|---|---|---|---|
| 11 | `forum.db`, which holds every account's secret key, and its WAL were created `0644`, inside a directory created `0755`: on a machine with other users, and a home directory they can enter, they could read the keys | The file and its WAL are `0600`, made so and tightened on open for a database an older version made; the data directory is `0700` | `the_store_file_is_readable_by_its_owner_only` |
| 12 | One unsigned request a minute for a range holding nothing (`since` in the far future, or `until` below the oldest post) was answered with nothing and still started every node's one-minute cooldown, so no node answered a newcomer. A narrow request also held the single request slot, so a newcomer's request arriving meanwhile was dropped | See the table above: nothing sent, no cooldown; a wider request takes the slot; a request made during the cooldown is answered when it ends | `a_request_for_nothing_does_not_silence_a_node`, `unsigned_requests_cannot_keep_a_newcomer_from_being_answered`, `a_request_during_the_cooldown_is_answered_when_it_ends` |
| 13 | A replayed bundle naming a pending request was compared with our first page by hashing every post of the bundle once per post of the page: one 140 KB bundle of 500 replayed posts cost about 180 ms on the UI thread instead of 20 ms, as often as it was sent | Each id is hashed once and looked up | `checking_a_bundle_against_a_pending_answer_costs_linear_work` |
| 14 | `forum.log`, the file a user attaches to a bug report, named every post this install sent ("handed 861d20192c4d to delivery…", "confirmed…"), anonymous ones included: a shared log said which anonymous posts were yours. In `forum.db`, a post written here was stored as received at the very millisecond of its date, which marked it too | The log says "a post", with no id and no delivery request id; a post written here is stored as received a moment after its date, as one from the network is | `an_anonymous_post_written_here_is_stored_like_one_received`; the log by reading every `log(` call in `src/logos_forum_backend.cpp` |
| 15 | In the topic list, an alias post showed its alias with no mark, so an alias written like someone's key (`3694…b945`), "anonymous", or the label of one of your own accounts read exactly like that key, an anonymous post, or your own post. An alias could also carry bidi overrides, zero-width characters and line breaks, which reorder or hide the text shown after it | The topic list marks an alias, "Ghost (alias)" (the thread already shows a badge and the real key), and an alias is shown without bidi controls, invisible or control characters, with runs of whitespace as one space. Authors are told apart by key everywhere; the display name is never compared | `an_alias_cannot_hide_or_reorder_what_is_shown_next_to_it`; `qml tests/qml/Harness.qml -- 800 600 authors` |
| 16 | The plugin counted the store twice for every message received, of any kind, to report what it added; this defeated the one-count-a-second bound for requests | The engine reports what it stored | `what_a_message_added_is_known_without_counting_the_store` |

Checked and found sound, with no change:

- **Deeply nested or oversized messages.** 150 KB of `[`, 25,000 nested
  objects and a post over the body limit are dropped without harm, under ASan
  and UBSan (`a_deeply_nested_or_oversized_message_is_dropped_without_harm`):
  nlohmann-json 3.11 parses and destroys iteratively.
- **Anonymous keys after a restore.** An anonymous key comes from libsodium's
  random generator at the moment of posting and is never stored or derived
  from stored state, so restoring `forum.db` from a backup cannot make two
  anonymous posts share a key.
- **Rotation.** A rotated key is generated fresh, not derived from the old
  one; nothing on the network is signed by both.
- **Back-dated posts.** The send limit counts the local clock, not a post's
  date, and a post dated in the past only sorts lower; one dated in the future
  is refused.
- **Accounts.** All accounts of an install belong to its one user. A post is
  marked as yours only if its key is one of your accounts' current keys, never
  by its name, and anonymous posts are never marked.

## Links in posts

Since 0.3.4 an https link in a post body or a topic's title opens in the
system browser when clicked. Showing it fetches nothing:

- The whole text is HTML-escaped first (`&`, `<`, `>`, `"`, `'`), so no tag
  written by the post's author survives. Only then are `https://` URLs found
  (up to whitespace or an escaped `<`, `>`, `"` or `'`, without trailing
  `. , : ; ! ? ) ] }`), and each becomes `<a href="URL">URL</a>` built from the
  escaped URL. Line breaks become `<br>`. Nothing else is ever produced, so
  there is no `<img>` to load.
- Plain `http://` URLs are not links: they are shown as ordinary text, and so
  are `javascript:`, `file:`, `data:` and every other scheme.
- The text is shown as StyledText, never RichText, and text with no link stays
  plain text, as before.
- A link is opened only on an explicit click, and only if it is an https URL
  as the forum wrote it; anything else (an http or other scheme, a broken href)
  opens nothing. Basecamp's QML sandbox refuses `Qt.openUrlExternally` for
  remote URLs, so the view hands the URL to the forum's backend, which checks
  it again (`is_openable_link`: https, a host, no user info, no whitespace,
  quote, angle bracket or control character, at most 2048 bytes; then parsed
  strictly) and starts the system's own opener with the URL as one argument,
  never through a shell (`open` on macOS, `xdg-open` on Linux,
  `url.dll,FileProtocolHandler` on Windows). Hovering shows the URL in a
  tooltip.
- Clicking does tell the site's owner your IP address, as any link does.

Checked by `scripts/check-no-remote-fetch.py`: https links to the check's own
server, some built to break out of the href, are rendered as links, plain
http URLs are not, and no connection is made without a click; a click opens
exactly the link's URL (the harness's stand-in backend only logs it); a StyledText `<img>` control is fetched,
so the format would load an image if one could get through. And by the unit
cases in `tests/qml/Harness.qml -- 800 600 linkify` (escaping, schemes, http
left as text, trailing punctuation, `<img src=x>https://a`,
`https://a"onmouseover=`, `javascript:alert(1)`), and the backend's check by
`only_an_https_link_can_be_opened` in the core tests.

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
  system's file permissions: the file and its WAL are `0600` and the forum's
  data directory `0700`, also for a database made by an older version
  (`the_store_file_is_readable_by_its_owner_only`). Anyone who can read the
  file as you, or as an administrator, has your account keys. A retired or deleted key is overwritten
  (`secure_delete`, the WAL is checkpointed after every rotation or deletion,
  and existing databases are rewritten once on upgrade), but a copy of the file
  taken earlier still has it.
- **The storage peer id is stable per install**, so with snapshots on, a peer
  that sees it twice can tell it is the same install. With them off (the
  default), it is never announced.
- **Shared modules.** Basecamp runs one `delivery_module` and one
  `storage_module` for all apps. The forum pins `delivery_module ~0.3.0`, the
  version it is tested with (the same line Chat 0.3.0 needs), so it cannot sit
  next to an app that needs another line. Whichever app initialises `storage_module` first sets its settings for
  all; the forum sets only its network, data directory and port, and uses a
  node another app started as it is.
- **History from peers is slow for a large backlog.** The logos.dev store
  nodes keep the forum's history; a network whose store nodes keep nothing
  (as logos.test's did on testnet v0.2) depends on peers being online. Over Delivery, a large backlog arrives in pages of 256 KB about a
  minute apart, and each answering peer spends at most 1 MB an hour, so a
  backlog of several MB behind a single peer takes hours
  (`a_backlog_larger_than_the_hourly_budget_still_arrives`).
- **A stranger can slow history down, not falsify it.** Requests are unsigned,
  so anyone can spend the peers' hourly answer budgets with cheap requests, or
  keep answering with the same page, and newcomers then wait longer. No post
  can be forged, altered or hidden that way: every post is checked, and nothing
  a stranger sends moves the paging past posts not yet received.
- **Fresh keys are free, so arrival limits bound, not stop, a flood.** What
  a node stores from the network is budgeted per hour of arrival (see [What a
  node accepts from the network](#what-a-node-accepts-from-the-network)). A
  limit per key alone would stop nobody, since anonymous posts use a key each;
  the shared budget for unseen keys and the live total are what hold, and they
  hold for everyone: on logos.dev, which runs no RLN, a stranger with fresh
  keys can still take the 60 posts an hour that new keys share, and so crowd
  out honest anonymous posts for that hour, or fill the live total with keys
  made "known" one slot at a time. They cannot make the store grow faster than
  the budgets allow, and refused honest posts come back from the store nodes'
  archive. On logos.test, RLN also limits each membership to 100 messages per
  10 minutes. Stopping fresh-key floods outright needs something every post
  carries (a proof of work, or RLN on every network), which 0.3.x peers would
  refuse; it is a proposal, not part of this version. Every arriving message
  is still parsed and its signature checked, up to the network's message size.
- **Your own device knows what it sent.** A post waits in the outbox, in
  `forum.db`, until the network confirms it, so an anonymous post not yet sent
  can be told apart there. Once sent, a post written here is stored like one
  received (`an_anonymous_post_written_here_is_stored_like_one_received`), and
  `forum.log` never names a post this install sent.

## Snapshots over Mix (opt-in)

Since testnet v0.3 the store nodes keep history and Delivery re-delivers missed
messages on reconnect, so a newcomer gets history without any snapshot.
Snapshots remain, off by default, for a complete archive of a large forum.
They are meant to be **private**, but that path has not yet passed end to
end: in its recorded run
([`e2e-snapshot-mix-0.3.2.out`](e2e/e2e-snapshot-mix-0.3.2.out)) the fresh
node got the post, but through Delivery, not from the snapshot, and the run
ends in `FAIL`. The direct download (`LOGOS_FORUM_SNAPSHOTS_DIRECT=1`) passes
([`e2e-snapshot-direct-0.3.2.out`](e2e/e2e-snapshot-direct-0.3.2.out)), at the
cost of connecting to the provider. As designed: with `fetchSnapshots` on, the storage node joins the network's Mix
and a snapshot is fetched in two steps, its manifest then its content, both
tunnelled over Mix (`isPrivate`) and not re-served (`advertise=false`). The
provider is found through the DHT and never dialled, so it does not learn the
fetcher's address. `LOGOS_FORUM_SNAPSHOTS_DIRECT=1` goes back to a direct
download.

Storage then runs on the `logos.test` storage network: in `storage_module`
3.0.0 the Mix relay list for `logos.dev` has an entry with an empty `mixPubKey`
and Storage refuses to start with Mix there ("Failed to load Mix relay pool:
Invalid mixPubKey in pool entry"). `LOGOS_FORUM_STORAGE_NETWORK` overrides.

## RLN, and the forum's sponsor

Logos Delivery 0.3 rate-limits with RLN on the networks whose preset enables
it: `logos.test` (opt-in, `"network": "logos.test"` in settings.json or
`LOGOS_FORUM_PRESET`) does; `logos.dev`, the forum's default, does not.
On logos.test a node attaches a proof to every message it sends, and the proof
needs an active membership in the registry the preset names
(`logos:testnet:841312e9…c893`, on the LEZ testnet zone behind
`http://209.38.241.182:3240`; epoch 600 s). The sender's own node enforces it:
without a proof it does not publish. The preset leaves proof validation off on
relays, so a node that strips the check could still post past the limit; that
is the network's choice, not the forum's.

A membership costs native balance on that zone, which forum users do not hold.
So the forum's author runs a **sponsor**: an open gifter (LIP-158,
[`logos-rln-gifter`](https://github.com/logos-co/logos-rln-gifter)) that
registers a membership for any node that asks and pays for it. On logos.test
the forum asks by itself on first launch, and the user never holds a token or
signs anything.

**It does not pay today.** Its account holds less than the fee reserve a
registration needs, and the zone's sequencer refuses its registrations with
`Incorrect fee` (`wallet_ffi` error 18,
[`e2e-sponsor-0.4.0-logos.test-unfunded.out`](e2e/e2e-sponsor-0.4.0-logos.test-unfunded.out)).
Every step before the payment works, and the forum shows the refusal; but
until the sponsor is funded, a node on logos.test gets no membership and its
posts wait in the outbox. The catalog's package runs on logos.dev and does not
bring the RLN modules.

How a node gets one:

1. delivery starts `liblogos_rln_module` with the preset's registry; the
   forum reads which registry and application scope delivery proves against
   (`rlnState`).
2. The forum brings up a plain libp2p node (`libp2p_module`, dialling out
   only) and calls `liblogos_rln_module.register_membership` with
   `delegated: true` and the sponsor's address. The RLN module generates the
   identity in-module, seals its secret in its keystore, and hands only the
   identity **commitment** to `rln_gifter_module`, which sends it to the
   sponsor over `/logos/rln/membership/1.0.0`.
3. The sponsor registers that commitment at rate 100 (the registry's minimum:
   100 messages per epoch), paying the price (1,000,000 units) and the fee
   from its own account, and answers with the leaf index. The node's RLN
   module watches the registry until the membership is active, then delivery
   proves with it like any other membership.
4. If the sponsor fails (unreachable, out of funds), the banner says so and
   the forum asks again after 1, 3, 10, then 30 minutes, or at once with
   *Try again now*. A membership is active 30 days, then 7 days of grace; the
   forum asks for a new one when none is usable.

The sponsor's address is built into the forum
(`/ip4/88.160.11.28/tcp/24026/p2p/16Uiu2HAm4XsEj65CPBnXZTZbniEE9SEiRtJUmngGuQxQoGFSHxA6`);
`"gifter": "<multiaddr>/p2p/<peer id>"` in settings.json, or
`LOGOS_FORUM_GIFTER`, names another one, and `"gifter": "off"` goes back to the
RLN module's own registration, which waits for its LEZ account to be funded
(the forum then shows that account and the amount).

**What the sponsor sees.** The IP address and the libp2p peer id of the node
asking (a fresh one at every start: the client node has no fixed key), the
identity commitment, and when. It registers the commitment from its own
account, so it is public on the registry's zone that the sponsor paid for that
leaf. It could log which address asked for which commitment.

**What it cannot see or do.** It never sees the identity secret, which stays
sealed in the node's RLN module, nor any forum key: forum posts are signed with
the forum's own Ed25519 keys, which have nothing to do with the membership.
It cannot post as the node, and it cannot tell which messages a membership
sends: an RLN proof shows that its sender holds *some* leaf of the tree, not
which one. The one exception is RLN's own penalty: a node that spends the same
message slot twice in one epoch reveals its secret, and with it its
commitment, which the sponsor could then tie to the address that asked. The
RLN module never reuses a slot, which is why its keystore must be moved and
never copied between installs. The sponsor learns nothing about what a user
reads.

**Open, with no limit.** The sponsor has no allowlist and no cap, by the
author's choice: whoever asks gets a membership, and asking again with a new
commitment gets another. Anyone can therefore spend the sponsor's balance.
When it runs out, new nodes see "The sponsor cannot pay for a membership right
now" and keep reading; posts wait in the outbox, and memberships already
granted are not affected. What the sponsor spends is visible on chain
(`scripts/gifter-status.sh` shows its balance and how many memberships it
still covers).

**Code the forum brings.** `liblogos_rln_module` 0.10.0,
`liblogos_lez_rln_module` 4.2.1 and `libp2p_module` 1.1.0 come from the
official Logos catalog. `rln_gifter_module` 0.2.1 is not in it: the forum's
catalog serves a build of `logos-rln-gifter` at commit c6d854a with
[`gifter/rln-gifter-module.patch`](../gifter/rln-gifter-module.patch), made by
this repository's CI. The patch does two things: it registers through
`liblogos_lez_rln_module.register_member`, where that method lives since the
RLN module 0.10.0 split, and it reports a registration the sequencer refused
as a failure instead of granting a leaf that will never confirm. The client
half the forum uses (`request`, `libp2p_call`) is unchanged.

- The membership belongs to the node, not to a forum account: one per
  Basecamp install, shared by every app that sends through its delivery node.
- Without a membership nothing is lost: posts are kept, delivery gives up on
  each send after its retry window, and the forum requeues it with back-off.
  Reading and history from store nodes need no membership.
- When the epoch's quota is spent, delivery holds the message
  (`messageQueued`) and sends it when the quota refills; the forum marks the
  post held until it is confirmed.

## Running nodes on one machine

Tests run several Basecamps on one machine, which can reach each other only
through loopback or LAN addresses. `LOGOS_FORUM_LOCAL_PEERS=1` lets such a node
announce and dial those addresses. Never set it on a node that talks to the
public network.
