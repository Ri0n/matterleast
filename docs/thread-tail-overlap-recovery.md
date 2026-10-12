# Thread tail overlap recovery

When a newest-boundary thread page contains a previously confirmed reply ID,
that overlap proves the page's ordinal placement independently of the root's
`reply_count`. If the server's tail would be placed too far to the right, and
all logical slots beyond the proven newest position are empty, the source may
trim precisely those phantom slots. No confirmed identity may be discarded.

The source retains the verified count correction across root-summary updates.
This is separate from a deletion of an unmapped reply inside a known newest
suffix (PR #186). Without an overlap, an empty or short transport page is
not evidence that `reply_count` is wrong.

The `THREAD_TAIL_OVERLAP_REANCHOR` warning records the old/new count and
adjustment. `ThreadPostSourceIntegrationTest` includes the production-like
28-slot mismatch and verifies that later live replies append normally.

## Live reply anchored beyond a shorter server-proven suffix

Root `reply_count` can overstate visible replies while a live reply is
already mapped at the final, inflated index. Example: a confirmed cursor at
266, live reply at 270, but the server responds to an after-266 request with
only [267, 268] and `has_next=false`. Ordinary conflict rejection strands
267–269 as missing rows.

A **forward** page after an authoritative anchor may correct this only when
`has_next` is explicitly known and false, the newest page ID matches the
rightmost confirmed reply, and *every* mapped suffix identity appears in the
response in the same relative order. The source first relocates identities
through its normal exact-window path, then trims only proved phantom slots.
Unexpected identities (including a concurrently appended reply), missing
`has_next`, or out-of-order overlap prevent this repair. Retain the corrected
metadata bias for subsequent root summaries and live appends.

`THREAD_CURSOR_TAIL_REANCHOR` records the correction; the integration test
reproduces the 266/270 conflict, incomplete-cursor refusal, and later append.
