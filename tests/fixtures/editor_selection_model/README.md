# Editor selection model contract fixture

This small .NET console fixture links the production `EditorModel.cs` and
`ComponentEditorModel.cs`. Only their native transport and independent gameplay
configuration dependency are replaced. It runs without Avalonia, a native engine,
rendering, input injection or window focus.

The fake transport keeps actual session selection separate from cached
`NativeHost.State`, retains exact authored entity documents and transaction
envelopes, validates original revision guards, and supports explicit RPC failures.
It does not contain a second selection reconciliation implementation.

Scenarios cover retained authored and live-component drafts, explicit resolution,
latest nullable remote selection, deleted entities, stale cached observations,
original-target transactions, failed reads and fail-closed gizmo commits. Live
component Apply/Reload/Discard and edit-back resolution exercise the actual
production component model. Selection-only scenarios compare complete fake
authored documents, revision and transaction history before and after navigation.

Run with an installed .NET 10 SDK:

```sh
dotnet run --project tests/fixtures/editor_selection_model/Poima.SelectionModel.Contract.csproj
```

The final stdout line is a JSON report with measured scenario/check totals and
per-scenario results. A failed scenario produces a nonzero exit code; remaining
scenarios still run so failures are independently visible.

For a regression comparison, a qualification runner may privately copy the project
and substitute its linked `EditorModel.cs` with a retained pre-fix source, using a
separate output/intermediate directory. The pre-fix model is expected to fail
dirty-draft resolution, live-component selection deferral and read-failure
synchronization cases. That expectation is not an execution result. Retain the
actual baseline report separately; never relabel its failures as current passes.

This fixture qualifies client state transitions only. Native bridge execution,
presented Inspector/Hierarchy/gizmo agreement, physical controls and docking
remain separate checks.
