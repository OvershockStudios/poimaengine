# Compiled physical recovery contract fixture

This fixture exercises the compiled recovery API through
`tests/physical_recovery_gameplay.py`. It is separate from Physical Character Yard.
The CoreCLR workflow passes on Windows and Linux with 34 complete saves and 19 whole-state comparisons on each platform.

The game implements `IPhysicalRecoveryGame`, which requests the 512-byte epoch 7
extension and only its physical-rig dependency. Its 28 scalar fields occupy 176
bytes. Actions 0/1/2 observe, activate and request recovery through the real SDK.
Every readback field, optional presence and support-body ID is recorded. Reads
after a staged request must remain equal to the committed pre-Tick observation.
`RejectAfterWrite` and `FailAt` exercise atomic callback and later-Tick failures.
A separate class in the same assembly uses only `IPhysicalRigGame` and attempts
the new request, testing actual permission denial rather than synthetic pointers.

The Python driver builds nothing. It imports a deterministic four-node skinned
triangle with the native three-proxy recovery fixture's pelvis, feet and unmapped
accessory. It authors a real linked CharacterController, floor, wall and blocker
template. There is no hidden native test mode. It requires actual successful
return, exclusive capsule/body ownership, complete first-boundary pose continuity,
subsequent walk and jump, fresh obstruction refusal, rollback and full Ready,
mid-transition and finished save continuations in a second process. All fields,
complete save bytes, original RPC streams, identities and failures are retained.

An external process-tree owner is required. Bounds are 360 seconds,
1,500 RPC calls, two sequential CLI owners and 256 MiB of output. This fixture is
CoreCLR-only; it does not qualify NativeAOT, original-Knight motion, rendering,
balance, get-up or performance. Actual result populations must come from runs.

The project resolves the repository root automatically. Use the exact SDK
copied with the selected bridge:

```text
<dotnet> build tests/managed_physical_recovery_gameplay/Poima.ManagedPhysicalRecoveryGame.csproj -c Release -p:PoimaSourceRoot=<checkout> -p:PoimaGameplayAssembly=<qualified-sdk> --artifacts-path <fresh-artifacts>
<python> tests/physical_recovery_gameplay.py <qualified-cli> --hostfxr <hostfxr> --bridge <qualified-bridge> --assembly <actual-fixture-dll> --output <fresh-output>
```

Do not remove native assertions or edit saved state if the authoring or current
runtime rejects the fixture. Keep the original failure, identify the source/API
mismatch and prepare a reviewed successor before another admitted run.
