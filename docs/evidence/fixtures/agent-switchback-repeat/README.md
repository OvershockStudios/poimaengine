# Additional agent-authored Switchback source

This independently authored implementation combines native crouch movement, camera-ray target queries, imported Idle/Run animation, independent world/viewmodel ADS, reloads, audio cues, compiled menus and checkpoints. [Original C#](src/RangeGame.cs) is preserved exactly: `fb2a36e301b9b29a5d273f87898502b690b99c1de827f08666f7a7f1e078c19d` (10,942 bytes). [Provenance](provenance.json) records source identity and shared content.

The unchanged public verifier passes thirteen groups against its original source project and its relocated readonly game service: nine complete version-9 checkpoints and five typed-exact state pairs in each campaign. Ready/Idle/Run Vulkan captures verify actual weighted skinning and committed patrol motion. [Results and limits](../../m2-agent-switchback-repeat.json).

The original provider process exited successfully, but its workflow failed qualification after a forced Script Job cleanup and missing historical native-owner proof. Later independent game checks preserve that failed verdict. The repeated fresh-workflow Alpha gate remains open.

[exercise.json](exercise.json) maps the game's actual actor, UI and imported-node identities for the public verifier. The common [exercise rules](../agent-switchback-fresh/TASK.md) apply. Its completion UI uses `Checkpoint: completed / generation ` for both successful Save and restored Save state; native ticket/generation/restore validation remains separate.

This is a source/evidence record. The evaluated original source project is distinct from the existing playable fresh fixture; a portable project reconstruction/rebuild for this implementation has not been qualified. The three selected cooked assets are byte-identical to those already in the [fresh fixture](../agent-switchback-fresh/README.md), with original CC0 character/interface notices retained there. No compiled image or machine-specific project references are distributed here.
