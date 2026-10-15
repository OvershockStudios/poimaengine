# Poima Python client

A standard-library client for Poima's native world service. It launches an
explicit native executable or attaches through its shared endpoint, with bounded
JSON-RPC transport, guarded authoring helpers, revision-pinned queries and
authoring-core v1 response validation. Registered runtime component queries
carry one observation boundary across all pages. It never retries mutations automatically.
Build-job helpers inspect one accepted job and wait under a finite budget, using
compact progress reads by default while preserving native terminal outcomes.

Opt-in runtime game-session v1 result checks use a frozen packaged manifest for
19 runtime/state/query/UI/save methods. Import `validate_game_session_result`
and `game_session_response_contract` from `poima_client`; pass the original
request and trusted component/gameplay schema context for contextual checks.
The host's discovery identity separately reports qualified target and scope.
The prospective 21-method workflow validator remains a different boundary.

From the engine checkout: `python -m pip install ./tools/python`.
An existing Poima executable is required; this package does not install the
engine or provider credentials. Python is external automation tooling; engine
and shipped gameplay remain compiled native code.

See the [client guide](https://github.com/OvershockStudios/poimaengine/blob/main/docs/PYTHON_CLIENT.md)
for examples, failure recovery, platform paths and current qualification limits.
The client and authoring-core contract are in development.
