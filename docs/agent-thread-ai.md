# ACP agent integration: thread-scoped AI chat

## Build and enable

The agent client is an experimental, **compile-time opt-in** feature:

```cmake
-DMATTERLEAST_ENABLE_AI=ON
```

The default is **OFF**: agent C++ sources and the `matterleast-mcp-stdio`
bridge are not compiled, and no AI tab or menu action is shown. Even when the
feature is compiled, **Settings → AI → Enable AI agent integration** is disabled
by default. No local AI listener or subprocess is started until an AI thread
window is opened after explicit opt-in.

Configure an **ACP v1 stdio agent executable** and its arguments in Settings → AI.
For a local Hermes ACP installation, use:

```text
Executable: hermes
Arguments:  acp
```

Hermes requires its ACP dependencies to be installed. Other ACP-compatible
agents can be selected without changing the MatterLeast binary. MatterLeast
does not choose a model or store model-provider credentials: use the agent's
own configuration and authentication workflow.

## Protocol and lifecycle

MatterLeast launches one ACP subprocess per AI thread window and uses UTF-8,
newline-delimited JSON-RPC 2.0 on stdio. `stdout` is protocol only;
`stderr` is diagnostics, not interpreted as ACP.

1. `initialize` negotiates the stable ACP protocol version **1**. MatterLeast
   advertises no file or terminal capabilities; arbitrary requests for such
   operations receive JSON-RPC `-32601`.
2. `session/new` uses a dedicated local application-data working directory
   and supplies `mcpServers` with a single named `matterleast` stdio server.
3. `session/prompt` sends a text content block. The subprocess and session
   remain alive across consecutive turns; no `--oneshot` or Hermes JSONL flags.
4. `session/update` message chunks update one assistant transcript entry in
   place. Tool status and thought updates affect the status line rather than
   inserting extra replies. The full final response is shown with a 3,000
   character display limit; a longer response is visibly truncated.
5. `session/request_permission` creates an explicit permission dialog.
   MatterLeast never automatically grants agent-requested operations. On user
   denial, cancellation or window closure, pending requests are cancelled.
6. **Stop** sends `session/cancel`, waits for the prompt completion result,
   then allows another turn. Closing the dialog stops its subprocess and
   revokes the thread-scoped MCP token. A stalled cancellation fails closed.
7. **Reconnect** may use `session/resume` or `session/load`, only when
   those capabilities are advertised by the agent, with the same MCP
   configuration. Unavailable or failed resumption is reported rather than
   silently dropping the old session context.

The supported wire format is ACP **v1**. ACP v2 is still a draft with breaking
protocol changes and is not negotiated by this implementation.

## MCP server attachment (automatic)

The user **does not** need to register MatterLeast MCP manually in Hermes.
Every ACP `session/new` (and resume/load when supported) includes:

```json
{
  "cwd": "/absolute/application/data/agent-workspace",
  "mcpServers": [
    {
      "name": "matterleast",
      "command": "/absolute/path/to/matterleast-mcp-stdio",
      "args": [],
      "env": [
        { "name": "MATTERLEAST_MCP_SOCKET", "value": "<local socket>" },
        { "name": "MATTERLEAST_MCP_TOKEN", "value": "<per-dialog token>" }
      ]
    }
  ]
}
```

The example paths and token are schematic. Actual values are generated for the
current MatterLeast installation and active AI thread.

The installed `matterleast-mcp-stdio` executable proxies MCP stdio JSON-RPC
to MatterLeast's `QLocalServer`; it does not load the account login token.
The socket uses local-user access where supported. Each request is checked
against a random capability, signed-in user, server identity, selected channel,
selected thread root and the separate write-permission toggle.

| MCP tool | Arguments | Scope |
| --- | --- | --- |
| `get_thread` | `limit` (1–100), optional `cursor` | Chronological paginated thread with root post |
| `get_post` | `post_id` | A single post belonging to the thread |
| `post_reply` | `message` | Write a reply only after explicit write opt-in |
| `reply_to_post` | `post_id`, `message` | Write a quoted thread reply only after write opt-in |

The agent's ACP permission prompt is distinct from MatterLeast's MCP write
toggle. Neither bypasses the other. The server enforces scope regardless of
prompt instructions. Thread content returned to the agent is untrusted data.

## Remote and corporate agents

ACP v1 standardizes **local stdio** agent-to-client transport. A corporate
Docker image exposing only a web page is **not** an ACP endpoint. To use such an
agent, the corporation must provide an ACP-compatible local command/helper or
an explicitly implemented remote ACP tunnel. A remote container also cannot
connect to a local `QLocalServer` without a transport bridge: the MCP
command specified in `session/new` runs on the *agent's host*, and paths must
be accessible there. This implementation does **not** open a network-reachable
unauthenticated MCP listener.

ACP subprocesses may have their own file/network tools. Passing a working
directory is *not* an operating-system sandbox. Do not use an untrusted agent
or grant sensitive permissions without reviewing its behavior.

## Test coverage

CI builds the standard AI=OFF variant and an explicit AI=ON variant. The
AI=ON tests start a deterministic fake ACP subprocess and verify initialization,
MCP configuration, streaming, permission approval, refusal of unadvertised
client methods, cancellation, and session resume without model credentials.
