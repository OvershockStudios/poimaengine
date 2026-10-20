# Compiled automatic-contact control consumer

This original test game requests the additive 368-byte grounding-control service
and character-input service. It reads both original Knight automatic slots,
stages complete enable/weight multipliers, checks same-callback reads still see
committed controls, performs one real controller Jump and can fail on a later
Tick. Manual IK remains forbidden for these slots even while disabled.

Build this project with the matching SDK and run it through
`tests/runtime_grounding_control.py --hostfxr ... --bridge ... --assembly ...`.
The Python fixture builds nothing; retain its raw requests/replies and complete
save payloads under an external process-tree owner. No rendered gait, foot
locking, physical input or performance claim follows from these checks.
