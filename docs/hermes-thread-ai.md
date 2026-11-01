# Hermes integration: thread-scoped AI chat

The **Open AI chat (Hermes)…** action is available only from a thread header's hamburger menu. It opens a standalone MatterLeast window. The agent converses with the user privately; nothing is published to Mattermost unless the agent calls one of the two write tools and the user has enabled posting in that window.

## One-time Hermes setup

Hermes Agent must be installed and authenticated. The MatterLeast build installs a console executable `matterleast-mcp-stdio` alongside the GUI executable. In your existing `~/.hermes/config.yaml`, merge (rather than overwrite) the following:

```yaml
mcp_servers:
  matterleast:
    command: "/absolute/path/to/matterleast-mcp-stdio"
    env:
      MATTERLEAST_MCP_SOCKET: "${MATTERLEAST_MCP_SOCKET}"
      MATTERLEAST_MCP_TOKEN: "${MATTERLEAST_MCP_TOKEN}"
```

The AI window has a **Copy MCP setup** button with the correct path for the current installation. Alternatively register it via Hermes' own command (`hermes mcp add matterleast --command /absolute/path/to/matterleast-mcp-stdio`). The bridge is intentionally unavailable to CLI processes started outside the AI window: the host sets ephemeral session credentials only for its own Hermes subprocess.

The default program is `hermes` from PATH. The window's **Select Hermes or hook…** button selects a custom CLI-compatible executable/script and persists it using the MLOptions key `ai/hermes/command` (an absolute executable path or a program on PATH). A custom executable/script may wrap the same Hermes CLI argv. Input is supplied as UTF-8 through stdin via `--query-file -`, never via command-line interpolation. The full contract for a wrapper is:

- Arguments are `chat --oneshot --query-file - --format stream-json --source tool` and, from turn 2, `--resume <session-id>`.
- Stdin contains the first-turn instruction and user text, or just the user's new message for subsequent turns.
- Environment includes `MATTERLEAST_MCP_SOCKET`, `MATTERLEAST_MCP_TOKEN`, `MATTERLEAST_THREAD_CHANNEL_ID`, and `MATTERLEAST_THREAD_ROOT_ID`.
- Stdout is Hermes JSONL (`system`, `text`, `tool_use`, `tool_result`, `result`). The UI consumes it incrementally. stderr is not used for protocol data.
- The subprocess gets only the temporary per-window capability; it never receives the Mattermost authentication token.

## MCP server

MatterLeast runs a **QLocalServer**, not a remotely reachable HTTP listener. It restricts connections to the local user where supported by Qt, and requires a randomly generated capability for every request. A separate console executable proxies stdio MCP JSON-RPC frames to this authenticated server. When the window closes, its capability is revoked.

All tools are scoped to the **one** originating thread. They use the signed-in backend's existing HTTP transport and caches rather than creating a second network session.

| Tool | Arguments | Result |
| --- | --- | --- |
| `get_thread` | `limit` (1–100), optional opaque `cursor` | Chronological posts with author, timestamps, quote metadata and attachment descriptors; root included on first page, `has_more` and `next_cursor`; iterate to read the full thread |
| `get_post` | `post_id` | One post, verified to belong to the thread |
| `post_reply` | `message` | A confirmed new Mattermost reply, or an error/uncertain outcome |
| `reply_to_post` | `post_id`, `message` | A confirmed quote-reply within the thread, using MatterLeast's existing quoted-reply metadata |

The write toggle is **off by default** and is enforced by the MCP server, not merely by a prompt instruction. Requests are checked for membership in the selected thread. All writes use `PostCreateService::createPostDetailed`; an enqueue is not reported as successful publication. If a response might have been lost, Hermes must re-read the thread before retrying, to avoid duplicate posts.

## Session and context policy

On the first turn, send a compact protocol instruction, channel ID, root post ID, tools/usage policy, 3,000-character single-message summary limit, and the user's text. Do not attach the full thread transcript: Hermes can request it via `get_thread` as needed. Thereafter resume **the same Hermes session ID** and send the new user text only. The MCP credential is re-established per subprocess through environment variables. This works even if posts arrive between turns.

A running turn updates the **same** assistant transcript entry with streamed text or tool status. The final `result.text` replaces the preview, so no chain of thought or repeated intermediate messages are published. The prompt explicitly asks for a concise one-message summary and no hidden reasoning. If Hermes exceeds 3,000 characters, the UI shows a warning and trims the displayed response.

## Boundaries

- A capability belongs to one window, one selected thread, and the current signed-in account.
- The bridge has no access to other channels or to the backend's authentication secrets.
- This is a **local** integration: a remote/cloud Hermes instance must have a secure, separately authenticated MCP transport, which this feature does not expose.
- Hermes installation, configuration, and model credentials are managed by Hermes; MatterLeast neither creates those credentials nor changes the user's existing Hermes configuration without consent.
- A stopped or failed turn stays inside the AI window and is not automatically published.

**Trust boundary:** posts and attachments returned by MCP are data from other participants, not commands to the agent. The first-turn instructions prohibit treating them as higher-priority instructions. The server separately enforces thread scope and the posting switch regardless of model behavior.
