# Native MCP interface (development)

Poima exposes its authoritative world operations through a compiled stdio MCP server. It uses the same world service as the CLI and editor, including schema validation, revision conflicts, undo and durable retry receipts. No Python or Node runtime is required by this server.

Linux native protocol tests pass. Real subprocess authoring/persistence tests and shared headless endpoint tests pass on both Linux and Windows, including cross-client receipts, stale-edit rejection and detachment without host shutdown. [Initial protocol evidence](evidence/m2-native-mcp.json), [cross-platform authoring evidence](evidence/m2-mcp-shared.json). It does not embed an agent, manage provider accounts or replace the CLI. Bounded authoring exercises also pass with the official Codex and Claude Code CLIs; see [client setup and qualification](AGENT_CLIENTS.md). Simple rendered-image interpretation also passes for both clients; see [observation evidence](evidence/m2-agent-observation.json). Embedded chat and attachment to a live desktop endpoint require separate verification.

## Launch

For a standalone world, configure the MCP client to launch the engine executable with these arguments:

```text
poima mcp --world /absolute/project/world.json
```

The parent directory must exist. This process owns the world's writer lock until EOF or process exit. To work alongside an already running editor or headless host, attach to its named endpoint instead:

```text
poima mcp --endpoint poima-desktop --timeout-ms 30000
```

World and endpoint modes are mutually exclusive. The endpoint timeout accepts 100–600000 milliseconds and applies to connection establishment and each native exchange. Endpoint mode detaches when MCP stdin closes; it does not stop the host. Windows and Linux endpoints are separate: a Windows editor requires the Windows engine executable. See [shared sessions](SHARED_SESSIONS.md).

Startup failures and diagnostics go to stderr. Stdout contains only MCP messages. This is a trusted local authoring interface with the engine process's file access; it is not an isolation boundary for untrusted code. Configure permissions in the agent client as appropriate for the project.

## Tools and workflow

Only two tools are advertised, keeping the initial tool catalog small:

| Tool | Arguments | Result |
| --- | --- | --- |
| `poima_discover` | Optional `view` and `name`, as in [focused discovery](WORLD_SERVICE.md#focused-discovery-development). Defaults to `catalog`. | Available operation names, a selected schema or contract section. |
| `poima_call` | Required `method`, optional object `params` and capture-only `image`. | The native operation result or error. |

Start with discovery. Read the `invariants` section and relevant operation schemas, then inspect the world before editing. For example, call `poima_call` with `{"method":"world.inspect"}`. Submit guarded transactions using the returned revision and a caller-generated durable `request_id`. The adapter does not invent revisions, fill missing mutation identifiers or automatically retry failures.

Results include both structured JSON and a text serialization for client compatibility. Successful content is `{"result": ...}`. Engine errors become `{"error": ...}` with `isError: true`, retaining the native error code and any details. Malformed MCP envelopes and unknown MCP tools use protocol errors instead.

If the native response is lost or invalid, the result reports `outcome_unknown: true`. An edit may already have committed. Inspect the authoritative state before deciding whether to retry; preserve the original durable request identifier when the operation supports receipts. Editor controls and other operations without receipts must not be blindly replayed.

Shared editor restrictions still apply. Use `desktop.describe` through `poima_call` to discover editor operations. Captures retain their native file-based output. The image-observation extension below passed its Linux codec and MCP tests, including conversion of an archived SDL capture. [Image qualification evidence](evidence/m2-mcp-images.json). Windows engine, desktop bridge and image/MCP test fixtures also cross-compile. Fresh rendered image consumption and simple color identification pass with both tested agent CLIs. General visual reasoning remains unqualified.

## Image observations (development)

To receive an image as well as the native capture receipt, add `"image": {}` to `poima_call`. An optional `max_edge` sets the longest output edge, from 128 to 2048 pixels (default 1280). This adapter option is not forwarded into native operation parameters.

Image observations support `world.capture`, `runtime.capture`, `asset.animation.capture`, `editor.capture` and completed `desktop.capture.status` results. Discover the native method first to obtain its required parameters. A queued desktop capture returns `observation.state: "pending"`; poll its status to retrieve the completed image.

Successful observations include a PNG MCP image content block and structured metadata with source/output dimensions and the source BMP SHA-256. The original native receipt remains in `result`; base64 image data appears only in the image block. Images are never enlarged. Unscaled conversion preserves RGB values; reduction uses area averaging in linear-light sRGB, so reduced observations are not pixel-identical evidence.

The decoder accepts the engine's uncompressed 24-bit or standard-mask 32-bit BMP captures, up to 4096 pixels per axis and 128 MiB of input. Extended headers must declare sRGB without an embedded or linked color profile. PNG output is bounded at 16 MiB. Receipt dimensions must match decoded source dimensions.

If capture succeeds but its file cannot be read or converted, the tool returns `isError: true`, retains the native `result`, and supplies `observation.state: "error"`. This is a known capture outcome, not `outcome_unknown`; do not repeat the operation merely to recover its receipt. No image conversion occurs for native error responses.

## Protocol and limits

The initial implementation negotiates MCP `2025-11-25`, with initialization, initialized notification, ping, tool listing and tool calls. It advertises tools only. It does not advertise resources, prompts, tasks, streaming progress or tool-list notifications. Unsupported requested versions receive the supported version so the client can decide whether to continue.

Input is UTF-8 newline-delimited JSON with a 1 MiB message limit and nesting limit of 64. Oversized lines are drained before the next message. Native responses are bounded at 32 MiB. Tool calls require completed initialization and an explicit request ID; notifications cannot mutate the world by invoking tools. Duplicate JSON keys are rejected.

Execution is synchronous and serial. Cancellation notifications do not interrupt an in-progress operation. Closing the process or timing out a request is not proof that a mutation was rolled back. These limits also apply when an agent client presents an asynchronous chat interface.

Protocol references: [lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle), [stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports), [tool messages](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
