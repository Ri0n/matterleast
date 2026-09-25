# Debug logging

MatterLeast uses Qt logging categories for high-volume diagnostic traces. Noisy
categories are disabled by default unless they are useful during normal
operation. Enable only the subsystem you are investigating.

## Running with logging rules

Qt reads category overrides from `QT_LOGGING_RULES`. Launch MatterLeast from a
terminal so both Qt logging and legacy stdout/stderr diagnostics are captured:

```bash
QT_LOGGING_RULES='mattermost.following.debug=true' \
  ./matterleast 2>&1 | tee /tmp/matterleast.log
```

Multiple rules are separated by semicolons:

```bash
QT_LOGGING_RULES='mattermost.timeline.*.debug=true;mattermost.following.debug=true' \
  ./matterleast 2>&1 | tee /tmp/matterleast.log
```

To enable all MatterLeast debug categories temporarily:

```bash
QT_LOGGING_RULES='mattermost.*.debug=true' ./matterleast 2>&1 | tee /tmp/matterleast.log
```

Prefer a narrow category during normal debugging: timeline and Following traces
can be very verbose on active servers.

## Useful categories

| Category | Default | Use |
| --- | --- | --- |
| `mattermost.upload` | info | Attachment selection, multipart transport, upload progress, HTTP response, returned `file_id`, and handoff to post creation |
| `mattermost.following` | warning | Following/Attention thread queries, pagination, merge statistics, unread state |
| `mattermost.timeline.trace` | warning | Virtualized list requests, visible/materialized ranges, navigation locks and read-cursor triggers |
| `mattermost.timeline.channel` | warning | Channel source paging, boundary reconciliation and logical placement |

Examples:

```bash
# Following / Attention
QT_LOGGING_RULES='mattermost.following.debug=true' ./matterleast

# Channel virtualization and navigation
QT_LOGGING_RULES='mattermost.timeline.trace.debug=true;mattermost.timeline.channel.debug=true' \
  ./matterleast

# Temporarily silence upload traces
QT_LOGGING_RULES='mattermost.upload.info=false' ./matterleast
```

## Attachment upload trace

A successful background upload should have the following high-level sequence:

```text
COMPOSER_ATTACHMENT_ADDED
COMPOSER_UPLOAD_START
UPLOAD_PREPARE
HTTP_UPLOAD_QUEUE
HTTP_UPLOAD_START
HTTP_UPLOAD_PROGRESS
UPLOAD_REPLY ... networkError=0 http=201 error=none
UPLOAD_ACCEPTED ... fileId=...
COMPOSER_UPLOAD_READY ... fileId=...
COMPOSER_SNAPSHOT ... fileId=...
COMPOSER_UPLOADS_READY
POST_SUBMIT ... attachmentIds=(...)
COMPOSER_SEND_FINISH
```

Useful failure boundaries:

- no `HTTP_UPLOAD_START`: request is stuck before the network transport;
- progress starts but no `UPLOAD_REPLY`: transport/server path did not finish;
- `UPLOAD_FAILED`: inspect HTTP status, network error and the bounded response
  body printed on that line;
- `UPLOAD_INVALID_REPLY`: the request succeeded but the response did not contain
  a usable `file_infos[].id`;
- `UPLOAD_ACCEPTED` exists but `POST_SUBMIT` has no matching `file_id`: the
  bug is in composer state/handoff rather than the upload API.

The trace prints whether authentication headers are present, but never prints the
Mattermost auth token itself. Each upload reply also records the Qt version,
SSL library build/runtime versions, negotiated TLS protocol/cipher, whether
Qt actually used HTTP/2, and sanitized request/response headers. Cookie,
authorization and CSRF values are redacted. These fields are intended for
comparing Qt 5/Qt 6 behavior with Chromium/Electron when a reverse proxy or WAF
rejects an upload before Mattermost returns its normal JSON response.

## Following / Attention

Enable:

```bash
QT_LOGGING_RULES='mattermost.following.debug=true' ./matterleast
```

Important records include:

- `query start` / `request`: which team/page/mode is queried;
- `response`: HTTP result and Mattermost thread counters;
- `parsed`: how many usable entries survived parsing;
- `state`: local DM/GM/thread unread projection;
- `merged`: final Following history + unread merge.

When investigating read-state problems, capture Following together with the
timeline categories:

```bash
QT_LOGGING_RULES='mattermost.following.debug=true;mattermost.timeline.*.debug=true' \
  ./matterleast 2>&1 | tee /tmp/matterleast-read-state.log
```

This allows a read-state report to be correlated with the exact viewport event
that advanced the read cursor.

## WebSocket events

Most high-frequency handled events should not dump their full JSON packet.
Typing events are deliberately silent. If the client prints:

```text
Unhandled WebSocket event '...'
```

keep the packet in a bug report: an unhandled event may represent missing client
state synchronization rather than harmless log noise.

For example, `multiple_channels_viewed` must be handled because Mattermost uses
its `channel_times` map to synchronize channel read state across client
instances.

## Filtering an existing log

Common quick filters:

```bash
grep 'mattermost.upload' /tmp/matterleast.log
grep 'mattermost.following' /tmp/matterleast.log
grep -E 'mattermost\.timeline\.(trace|channel)' /tmp/matterleast.log
grep -E 'Unhandled WebSocket event|WebSocket (error|disconnected|reconnect)' \
  /tmp/matterleast.log
```

When reporting a failure, include the trace from a little before the user action
through the final success/error event. For upload problems, include the complete
`mattermost.upload` sequence for one attachment.
