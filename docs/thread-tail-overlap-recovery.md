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
