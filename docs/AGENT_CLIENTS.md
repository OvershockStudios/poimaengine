# Codex and Claude Code

Poima's native MCP server can be used by the official Codex and Claude Code CLIs. The engine remains the authority for schemas, revisions, edits, receipts and runtime state. The clients supply the agent and their own authentication; Poima does not store provider credentials.

Bounded authoring exercises pass with Codex CLI **0.160.1** on Linux and Claude Code **2.1.290** on Windows. Both agents discovered schemas, created an entity, replayed the exact transaction, observed stale-revision rejection, queried state, then undid and redid the edit. A separate trace verifier checked the actual tool calls and persisted world. These exercises do not qualify general game creation, embedded chat or cancellation. A separate simple rendered-observation exercise also passes for both clients; see below. [Evidence](evidence/m2-agent-authoring.json).

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

## Rendered observations

Both tested clients receive Poima's MCP PNG image blocks. In a separate exercise, each identified the left/right colors of two rendered objects without being told the colors. A verifier checked that only discovery and one capture occurred, that the receipt hash matched the fresh BMP, that a PNG image block reached the client, and that the answer matched independently measured source pixels. [Observation evidence](evidence/m2-agent-observation.json).

The exercise uses a camera at `(0, 0, 5)` looking along local `-Z`, with two unit boxes at `(-1, 0, 0)` and `(1, 0, 0)`. Object names are neutral; choose two distinct dominant RGB albedos independently of the agent prompt. Ask for one `world.capture` at 640×400 with four samples and the adapter option `"image": {"max_edge": 640}`. Restrict this exercise to discovery and capture, then request a JSON answer containing `left` and `right` colors.

```text
python tests/agent_observation_trace.py codex EVENTS.jsonl CAPTURE.bmp
python tests/agent_observation_trace.py claude EVENTS.jsonl CAPTURE.bmp
```

This is one simple image per client on the NVIDIA development GPU. It establishes delivery and bounded interpretation, not a general vision benchmark, reduced-image accuracy or autonomous game creation. Poima's optional [MCP image parameter](MCP.md#image-observations-development) retains the native capture receipt alongside the image.

Official references: [Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp), [Codex app-server](https://learn.chatgpt.com/docs/app-server), [Claude Code programmatic use](https://code.claude.com/docs/en/headless). CLI options can change; the recorded versions above identify what was exercised.

## Embedded workspace transport (development)

`desktop/Poima.AgentHost` provides a reusable .NET 10 stdio JSON-RPC process transport with no external packages. It correlates concurrent requests, forwards provider notifications and approval requests, preserves server request IDs in replies, and bounds incoming frames and retained diagnostics. The owner must continuously consume its bounded event stream and explicitly handle approvals. Event overflow closes the connection explicitly instead of blocking response dispatch. A failed or canceled partial write also closes the connection, and concurrent disposal waits for the writer and process cleanup. Transport cancellation does not imply that a provider or engine mutation was rolled back; a missing response after sending is reported as an unknown outcome and is never automatically retried.

`CodexSession` adds official app-server initialization, persistent thread creation/resume, text turns, interrupt requests, paginated history and Poima MCP discovery. Its process-local MCP configuration attaches to an existing shared endpoint. Codex retains account ownership and its existing user configuration; Poima does not manage credentials or write global provider configuration. Approval requests remain explicit events for the caller to handle.

This is backend groundwork, not an embedded chat interface. A Linux integration check with Codex CLI 0.160.1 discovered both Poima tools, completed one short text turn, restarted the app-server process, resumed the same thread and retrieved the exact saved assistant response. Streaming notifications were received. Windows session integration, actual approval UI, interrupt completion, Claude's persistent adapter and desktop integration remain unqualified or unfinished. The interrupt response alone does not establish that a turn finished or an engine edit was undone.

The process-level contract fixture launches a synthetic peer and exercises concurrent correlation, Unicode, RPC errors, approval replies, cancellation, late responses, bounded diagnostics, truncated output, malformed responses, oversized frames, event overflow, cancellation during a blocked pipe write and concurrent disposal. It checks that disposal leaves no live peer process:

```text
dotnet run --project tests/fixtures/agent_rpc/Poima.AgentRpc.Contract.csproj
```

The fixture requires .NET 10. Its synthetic peer does not establish provider compatibility; the real-client qualifications above cover the separate MCP integration.

For the explicit provider-backed session check, first start a disposable shared host, then run:

```text
dotnet run --project tests/fixtures/agent_rpc/Poima.AgentRpc.Contract.csproj -- --codex CODEX_EXECUTABLE ABSOLUTE_POIMA_EXECUTABLE ENDPOINT WORKING_DIRECTORY
```

This uses the installed Codex account, performs one short model turn and creates provider-owned conversation history. It rejects server approval requests during the check. Raw provider history stays outside the repository.
