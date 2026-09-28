# End to end, between two real nodes

Two Logos Basecamp 0.3.0 instances on one Mac (`--user-dir` keeps them apart),
each with `delivery_module` 0.2.1 and `storage_module` 2.1.2 from the official
catalog, joined to the public **logos.test** network. The forum is driven
through its own interface; every check reads the node's own SQLite store.
Script: [`scripts/e2e-two-nodes.sh`](../scripts/e2e-two-nodes.sh).

| Step | What is checked | Result |
|---|---|---|
| A posts a topic | B stores it; the author key is identical in both stores | ✓ (~1 s) |
| B replies anonymously | A stores it with mode *anonymous*, under a key none of B's accounts hold | ✓ |
| B is wiped and restarted | B recovers both posts from A's snapshot on Logos Storage | ✓ |
| D, installed **from the catalog** on a blank Basecamp, starts | D recovers the forum from A's snapshot (storage 2.1.3 ↔ 2.1.2) | ✓ |
| D replies under an alias | A stores it as *alias* "Ghost of D", signed by D's account key | ✓ |

![B receives A's topic](e2e/1-b-receives-a-topic.png)
![A receives B's anonymous reply](e2e/3-a-receives-b-anonymous-reply.png)
![All three signing modes, as node A sees them](e2e/5-three-signing-modes.png)

## What the network keeps

Before relying on the network's history, we measured it. A Store query to
each of four logos.test fleet nodes (`node-01.do-ams3`, `node-01.gc-us-central1-a`,
`node-01.ac-cn-hongkong-c`, `node-02.do-ams3`) returns `200 OK` and **no
messages** — for the forum's content topic, for its whole shard
(`/waku/2/rs/2/6`), and with no filter at all. The fleet keeps no archive, so
a forum that depended on Store queries could never show a newcomer anything.

The forum still queries the store nodes (a network that archives will work
without change), and it also asks its peers. A node that comes online sends
`{"logos-forum-want":1, "have": <count>}` on the forum's topic. A peer holding
more posts saves a snapshot to Logos Storage and announces its CID, together
with its storage node's peer id and addresses. The newcomer dials that node
directly, downloads the snapshot, and merges it post by post — every signature
is checked, so a snapshot carries no authority of its own. Snapshots are
content-addressed: two peers holding the same posts announce the same CID, and
it is fetched once. A peer answers at most every 30 seconds.

From the fresh node's log ([`e2e/node-b-fresh-install.log`](e2e/node-b-fresh-install.log)):

```
catch-up (reconnected): 0 messages, 0 new, peer …do-ams3 (0), …us-central1-a (0), …hongkong-c (0), …node-02.do-ams3 (0), status 200 OK
asked peers for history
dialing snapshot provider 16Uiu2HAkyNSyKrUi1q64jR1Xme1bzoEj722LiQWmnbAo7rVhiYpV
fetching snapshot zDvZRwzm8N45C9mv9f1Xi32Voh1deqBnrUuhu1ZbjjnQgwBqd5Xp
download done: {"sessionId":"zDvZ…Xp","success":true}, 422 bytes
snapshot imported: 1 new
```

## Bugs this found

Running in the real host found three faults that no unit test could:

- A synchronous `storeQuery` blocked the backend; every module call that can
  take time is now asynchronous.
- A store node's last page carries `"paginationCursor": null`, and
  `json::value()` throws on a present-but-null key. An exception escaping a
  module callback aborts the ui-host process, so every read of network or
  module JSON is now typed and non-throwing — including in the core, which
  gained a test that feeds it mistyped traffic.
- A synchronous storage call made while storage_module was dispatching its
  own event waited out the 20 s RPC timeout; storage calls are asynchronous too.

Also avoided by design: no `QRegularExpression` anywhere (under Basecamp's
hardened runtime it JIT-compiles and traps), and the storage node listens on a
per-install random port, since the default discovery port is fixed and two
instances on one machine would collide.
