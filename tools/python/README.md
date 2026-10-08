# Poima Python client

A standard-library client for Poima's native world service. It launches an
explicit native executable or attaches through its shared endpoint, with bounded
JSON-RPC transport, guarded authoring helpers, revision-pinned queries and
authoring-core v1 response validation. It never retries mutations automatically.

From the engine checkout: `python -m pip install ./tools/python`.
An existing Poima executable is required; this package does not install the
engine or provider credentials. Python is external automation tooling; engine
and shipped gameplay remain compiled native code.

See the [client guide](https://github.com/OvershockStudios/poimaengine/blob/main/docs/PYTHON_CLIENT.md)
for examples, failure recovery, platform paths and current qualification limits.
The client and authoring-core contract are in development.
