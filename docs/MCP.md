# Native MCP interface (development)

Poima exposes its authoritative world operations through a compiled stdio MCP server. It uses the same world service as the CLI and editor, including schema validation, revision conflicts, undo and durable retry receipts. No Python or Node runtime is required by this server.

Linux native protocol tests pass. Real subprocess authoring/persistence tests and shared headless endpoint tests pass on both Linux and Windows, including cross-client receipts, stale-edit rejection and detachment without host shutdown. [Initial protocol evidence](evidence/m2-native-mcp.json), [cross-platform authoring evidence](evidence/m2-mcp-shared.json). It does not embed an agent, manage provider accounts or replace the CLI. Bounded authoring exercises also pass with the official Codex and Claude Code CLIs; see [client setup and qualification](AGENT_CLIENTS.md). Simple rendered-image interpretation also passes for both clients; see [observation evidence](evidence/m2-agent-observation.json). Embedded chat and attachment to a live desktop endpoint require separate verification.

## Launch

For a standalone project, configure the MCP client to launch the engine
executable with its manifest path:

```text
poima mcp --project /absolute/project/project.poimaproject
```

`--project` validates the existing project and resolves the entry world
relative to its manifest. It accepts legacy `project.json` files. Use
`--world /absolute/project/world.json` to work directly on a world, including
during project repair. Either standalone mode owns the world's writer lock
until EOF or process exit and supports `--development-profiles file.json`.
To work alongside a running editor or shared headless host, attach to its
named endpoint:

```text
poima mcp --endpoint poima-desktop --timeout-ms 30000
```

Choose one target per MCP process. The endpoint timeout accepts 100–600000 milliseconds and applies to connection establishment and each native exchange. Endpoint mode detaches when MCP stdin closes and leaves the host running. Use the Windows engine executable to connect to a Windows editor, including from WSL. See [shared sessions](SHARED_SESSIONS.md).

Startup failures and diagnostics go to stderr. Stdout contains only MCP messages. This is a trusted local authoring interface with the engine process's file access; it is not an isolation boundary for untrusted code. Configure permissions in the agent client as appropriate for the project.

## Tools and workflow

The tools expose discovery, world operations, source edits, gameplay checks
and build-job submission with separate approval for each kind of write.

| Tool | Arguments | Result |
| --- | --- | --- |
| `poima_discover` | Optional `view` and `name`, or mutation `operation` and `type`. Use `selection` on its own to [retrieve multiple exact schemas](WORLD_SERVICE.md#select-multiple-schemas). Defaults to `catalog`. | Available operation names or selected schemas and contract sections. |
| `poima_reference` | Empty arguments list guides. Set `guide` for its outline or add an exact `section` heading. Optional `line` and `lines` page the requested text. | Bounded Markdown, heading index or catalog from the current host build. |
| `poima_call` | Required `method`, optional object `params` and capture-only `image`. | The native operation result or error. |
| `poima_source` | `project.source.write` with project selectors, exact guard and replacement text. | A guarded write receipt and resulting SHA-256. |
| `poima_checks` | `project.verification.write` with project selectors, exact guard and complete suite JSON text. | A guarded suite write receipt and resulting SHA-256. |
| `poima_build` | Required build or verification `method` and object `params`. | A session-local job receipt for a trusted build or check run. Inspect it through the world API. |

For example, pass `{"guide":"creator-gameplay","section":"Publish and attach"}`
to `poima_reference`. The tool always invokes `world.reference` on the same
owner, using its bundled guides and preserving the guide hash and page limits.
It advertises read-only access to the client and accepts only the native guide
selectors. The client can permit these reads without granting any of the edit
or build tools. `world.reference` also remains available through `poima_call`.
[Guide selection and paging](CREATOR_GAMEPLAY.md)

Read editable C# with `project.source.read` through `poima_call`.
The host's profile and open world govern the source selectors.
[Source and build workflow](CREATOR_GAMEPLAY.md#write-compiled-c-gameplay)

Read the editable suite with `project.verification.read` through `poima_call`.
Check writes select only `game.verify.json` in the open project. The adapter
accepts up to 1 MiB per JSON-RPC request, including the encoded suite and
request envelope. [Suite authoring and execution](GAME_VERIFICATION.md)

[Host-configured development profiles](DEVELOPMENT_PROFILES.md) authorize native
publishing and export. Their execution tool requires separate provider consent;
compile, publish and export cannot use `poima_call`. Approval of ordinary world
edits does not approve build submission. World operations may still execute
selected trusted gameplay; this is not a general code-execution sandbox. The host
configures tools before accepting requests, and the adapter does not retry an
uncertain submission.

For a small mutation schema, call `poima_discover` with
`{"view":"mutation","operation":"component.set","type":"Transform"}`.
This keeps the transaction's revision/receipt requirements visible while omitting
provably unrelated operation branches. Fetch other selections or the complete
`world.transact` method when composing mixed edits. Existing views remain
available; malformed or unavailable selections retain native errors.

For a small authoring task, send one selection through `poima_discover`:

```json
{"selection":{"methods":["entity.query"],"components":["Transform"],"mutations":[{"operation":"entity.create"},{"operation":"component.set","type":"Transform"}]}}
```

The adapter forwards this to `world.describe.select`. The selected schemas
match single lookups, with contract and owner metadata sent once. Reuse the
schemas while the owner and schema revision are the same.

Start with discovery. Read the `invariants` section and relevant operation schemas, then inspect the world before editing. For example, call `poima_call` with `{"method":"world.inspect"}`. Submit guarded transactions using the returned revision and a caller-generated durable `request_id`. The adapter does not invent revisions, fill missing mutation identifiers or automatically retry failures.

Results include both structured JSON and a text serialization for client compatibility. Successful content is `{"result": ...}`. Engine errors become `{"error": ...}` with `isError: true`, retaining the native error code and any details. Malformed MCP envelopes and unknown MCP tools use protocol errors instead.

If the native response is lost or invalid, the result reports `outcome_unknown: true`. An edit may already have committed. Inspect the authoritative state before deciding whether to retry; preserve the original durable request identifier when the operation supports receipts. Editor controls and other operations without receipts must not be blindly replayed.

Shared editor restrictions still apply. Use `desktop.describe` through `poima_call` to discover editor operations. Captures retain their native file-based output. The image-observation extension below passed its Linux codec and MCP tests, including conversion of an archived SDL capture. [Image qualification evidence](evidence/m2-mcp-images.json). Windows engine, desktop bridge and image/MCP test fixtures also cross-compile. Fresh rendered image consumption and simple color identification pass with both tested agent CLIs. General visual reasoning remains unqualified.

## Review a shared revision

A lead can get the open world's ID and revision from `world.inspect`. Give
each reviewer the same endpoint, world ID and revision. Connect each
reviewer with its own MCP stdio process:

```text
poima mcp --endpoint poima-desktop --review-revision 12
```

This connection exposes `poima_discover`, `poima_reference` and
`poima_inspect`. Use `poima_inspect` with
`{"method":"world.inspect"}` to confirm the assignment. Other available
methods are `entity.query`, `entity.get`, `entity.world_transform`,
`world.lighting`, `world.asset.references`, `world.ui.list` and
`world.ui.get`. Pass their native arguments as `params` and use the
read-only `poima_discover` tool to find the current schemas. The adapter
sets the revision on each guarded request. If explicitly supplied, the
revision must match the assignment. Discovery and bundled guide reads
remain available throughout the review.

Report the inspected world ID, revision and entity IDs with the findings.
The lead uses its authoring connection to verify the proposals and submit a
transaction with the same `base_revision`. After an intervening commit,
inspections return `-32009`. Assign the new live revision in a fresh
connection and repeat the affected reads. The connection holds neither a
snapshot nor the writer lock. Closing its stdin detaches the reviewer.

This option applies only to shared MCP connections and accepts revisions
from zero through 2^53−1. It controls which operations the adapter can send.
Client shell and filesystem permissions are governed by the client.

## Edit a worker draft

Create the copy with [`project draft`](PROJECT_RECOVERY.md#create-worker-drafts-and-stage-an-integration).
Give each worker its own manifest and stdio connection.

```text
poima mcp --project /absolute/draft/project.poimaproject --draft
```

Poima checks the draft's lineage marker and project identity at startup. The
connection owns the draft's world writer. It exposes `poima_discover`,
`poima_reference` and a scoped `poima_call` for scene inspection, transactions
and asset authoring. For model, animation donor, image and audio imports, use
portable source paths relative to the draft entry world's directory. Use
discovery for the active method catalog.

Keep build and verification jobs with the lead's candidate workflow. After
the worker finishes a turn, review its report and compose the selected
operations and file hashes into a separate integration request.

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
