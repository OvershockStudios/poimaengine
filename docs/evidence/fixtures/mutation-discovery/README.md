# Mutation discovery measurement requests

These provider-free requests compare complete and focused transaction discovery.
They inspect the world before and after without authoring edits. Use a rebuilt
native engine and separate copies of the [defense world](../agent-defense/world.json)
for the two process owners. In a shell supporting stdin/stdout redirection:

```sh
poima world build/discovery-world.json < docs/evidence/fixtures/mutation-discovery/world.requests.jsonl > build/discovery-world.responses.jsonl
poima mcp --world build/discovery-mcp-world.json < docs/evidence/fixtures/mutation-discovery/mcp.requests.jsonl > build/discovery-mcp.responses.jsonl
```

The world stream returns nine JSON-RPC lines. The MCP stream returns three;
its initialized notification has no response. Require exit code zero, correlated
IDs, successful results, equal first/last world inspections and unchanged input
world bytes. Requests do not authenticate or contact an agent provider.

Count each native UTF-8 response line before adding its newline delimiter. The
MCP count includes both text and structured output. File redirection through
another encoding changes the count; record the engine's original byte stream.
These are response bytes, not provider tokens or development-speed measurements.
Reference-bearing or unfamiliar transaction schemas conservatively retain their
full union, so byte savings depend on the available schema. The
[checkpoint evidence](../../m2-scoped-mutation-discovery.json) records one machine's
native Windows/Linux measurements and qualifications.
