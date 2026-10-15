# Compiled Control component reads

This fixture exercises typed read-only component access from `ControlContext` at unchanged simulation time. It is independent of the management demonstration.

The authored setup contains a capacity-four native entity FIFO, four live work components, and one live entity without a work component. Each 320-byte work component contains all five scalar kinds and capacity-two buffers of all five kinds. Compiled controls check exact values, complete exclusive query pages, missing-component versus unknown-entity semantics, local-copy isolation and full-queue feedback without a Tick. Failure actions stage global/UI/save changes before a read error to exercise existing callback rollback.

The bounded Windows Native AOT qualification used native 0.0.149 and a freshly compiled SDK/module: 499 native requests, 485 selected successful-result checks against the released 19-method contract, five successful controls, three armed read-error rollbacks and one historical receipt retry, all at Tick 0. Raw native snapshots and whole typed payload predictions are separate from selected RPC observations. See the [qualification evidence](../../docs/evidence/m2-control-component-reads.json).

To repeat from a configured Windows x64 .NET Native AOT build environment, publish the fixture with the matching engine version, then run the public self-supervised verifier:

```powershell
python scripts/publish_native_gameplay.py --project tests/control_component_reads_gameplay/Poima.ControlComponentReadGame.csproj --type Poima.Tests.ControlReadGame --rid win-x64 --engine-version 0.0.149 --output build/control-read-artifact --work build/control-read-aot-work
python tests/control_component_reads.py --engine path/to/poima.exe --artifact build/control-read-artifact --output build/control-read-check --seconds 180
python tests/control_component_reads_contract_test.py
```

Use fresh output/work directories. The explicitly selected CLI must have its matching `phonon.dll` beside it. Publication needs an appropriate C++ linker/Windows SDK and matching .NET SDK; the successful producer's selected tools and warm package-cache limits are recorded in evidence. The public consumer compiles nothing. It pins the selected engine, complete published artifact and its public helper/SDK inputs, then runs native descendants inside a Windows Job and retains original requests, replies, snapshots and closure metadata under the output. It never retries or substitutes an unknown process outcome.

`read` observes committed state, so an ordinary guarded external component edit between controls is visible to the next control. The fixture supplies no component or lifecycle mutations from Control. Native services epoch 7 and the 176-byte baseline are unchanged; collections use the existing `component_collections_v1` feature.

This qualifies source-owner callbacks on Windows Native AOT with capacity-two buffers and a capacity-four queue. It does not qualify exported/readonly or CoreCLR consumers, all capacities, a genuinely retired handle, rendering, physical input, performance or complete Alpha. Unknown/null and live-without-component handles are tested. Local normal exits are joined to the supervising Job; input hashes are not a claim of OS sandboxing or complete toolchain reproducibility.
