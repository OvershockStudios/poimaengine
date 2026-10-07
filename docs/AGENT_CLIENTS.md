# Codex and Claude Code

Poima's native MCP server can be used by the official Codex and Claude Code CLIs. The engine remains the authority for schemas, revisions, edits, receipts and runtime state. The clients supply the agent and their own authentication; Poima does not store provider credentials.

Bounded authoring exercises pass with Codex CLI **0.160.1** on Linux and Claude Code **2.1.290** on Windows. Both agents discovered schemas, created an entity, replayed the exact transaction, observed stale-revision rejection, queried state, then undid and redid the edit. A separate trace verifier checked the actual tool calls and persisted world. These exercises do not qualify general game creation, embedded chat, cancellation or rendered-image interpretation. [Evidence](evidence/m2-agent-authoring.json).

## Connect a project

Use an absolute engine executable path and a world path whose parent directory exists. For a standalone world, the MCP process owns its writer lock. To work alongside an editor or headless host, use its endpoint instead; see [MCP](MCP.md) and [shared sessions](SHARED_SESSIONS.md). A Windows host requires a Windows Poima executable.

Codex accepts a stdio server in its MCP configuration:

```toml
[mcp_servers.poima]
command = "/absolute/path/to/poima"
args = ["mcp", "--world", "/absolute/project/world.json"]
required = true
```

For an existing host, replace `args` with `["mcp", "--endpoint", "your-host"]`. Poima advertises its general operation tool as potentially mutating. Consequently, even a read operation through `poima_call` can require client approval. `approval_policy = "never"` alone does not authorize those calls: it can reject them instead. For an explicitly trusted autonomous project, Codex supports a per-tool authorization:

```toml
[mcp_servers.poima.tools.poima_call]
approval_mode = "approve"
```

That authorizes all operations exposed through this tool, with the engine process's file access. Interactive use can retain approval prompts. Revision guards still apply regardless of client approval settings.

Claude Code accepts a server definition in a JSON configuration file:

```json
{
  "mcpServers": {
    "poima": {
      "type": "stdio",
      "command": "C:/absolute/path/poima.exe",
      "args": ["mcp", "--world", "C:/absolute/project/world.json"]
    }
  }
}
```

Launch the official CLI with `--mcp-config PATH`. For an isolated test, `--strict-mcp-config` restricts servers to that configuration, and `--allowedTools mcp__poima__poima_discover,mcp__poima__poima_call` pre-authorizes Poima's tools. These options do not sign the user in; use the provider's normal authentication flow separately. Use an executable built for the platform running it, especially when switching between WSL and Windows.

## Authoring exercise

In an empty temporary project, ask the agent to:

1. Discover the catalog, invariants and relevant method schemas.
2. Inspect the world, then create one entity named `Agent integration probe` with a guarded transaction.
3. Repeat the identical transaction and verify its replay receipt.
4. Submit a new request ID with the original stale revision and observe rejection.
5. Query the single entity, undo to an empty world, then redo to one entity at revision 3.

Capture Codex's `--json` events or Claude Code's `--output-format stream-json --verbose` events. The optional verifier performs no inference and does not call a provider:

```text
python tests/agent_authoring_trace.py codex EVENTS.jsonl WORLD.json
python tests/agent_authoring_trace.py claude EVENTS.jsonl WORLD.json
```

Provider-backed exercises are explicit integration checks, not part of the default test suite. Raw transcripts and account data do not belong in the engine repository. Qualification records contain bounded results and hashes.

Official references: [Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp), [Codex app-server](https://learn.chatgpt.com/docs/app-server), [Claude Code programmatic use](https://code.claude.com/docs/en/headless). CLI options can change; the recorded versions above identify what was exercised.
