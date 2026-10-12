# Thread reply-count provenance

The Mattermost root post's `reply_count` is the count of non-deleted replies,
excluding the root itself. It is a server-provided **snapshot**, not a count
of bodies already present in `BackendChannel::posts`.

## REST snapshot versus WebSocket arrival

`BackendPost` parses `reply_count` and `last_reply_at` from full root
post payloads. `BackendChannel::addPost()` handles newly received resident
WebSocket replies, while `mergePostContext()` ingests REST/cache windows
without increasing the root's reply_count for each fetched body.

**A reply identity new to the resident cache need not be new to the server's
reply_count.** For example, a REST root snapshot with `reply_count=3` and
`last_reply_at=t3` may reach the client before WebSocket delivers the body of
reply 3. Blindly incrementing `reply_count` for the newly resident ID creates
a phantom row and can make the timeline's newest confirmed ID disagree with
its supposed logical ordinal.

`BackendPost::_serverThreadLastReplyAt` therefore records the latest
timestamp from an actual root summary snapshot. Local live updates still
advance `last_reply_at`, but must **not** advance that server watermark.
When a new resident reply has `create_at <= _serverThreadLastReplyAt`, the
server snapshot already covers its timestamp, so it cannot be counted again.
When it is newer, count it once, independently of the order of later
WebSocket arrivals. A subsequent full root snapshot resets this watermark
even if its visible values already equal the locally advanced ones.

A root payload without both `reply_count` and `last_reply_at` provides
no reliable timestamp boundary; do not infer that a previously unseen reply
was included. The timestamp comparison also cannot distinguish separate
replies sharing an identical millisecond across the snapshot boundary;
identity/order reconciliation in `ThreadPostSource` remains the final check.

This prevents one source of inflated counts; it does not prove that a server
root count exactly matches every user's currently retrievable `/thread`
history (permissions, deletions and server-side lag can still differ).
The separate cursor recovery logic is documented in
[thread-tail-overlap-recovery.md](thread-tail-overlap-recovery.md).

## Tests

`BackendThreadSummaryTest` covers delayed WebSocket delivery of already
counted replies, genuinely newer live replies, out-of-order WebSocket events,
subsequent REST snapshot refresh, replayed reply IDs and missing server
timestamp metadata.
