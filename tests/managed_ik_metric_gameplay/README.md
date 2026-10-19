# Original Knight metric consumer

Build this original test game with the current SDK and supply its assembly to
`tests/runtime_ik_metric_profile.py` together with a qualified CoreCLR bridge and
hostfxr. The Python fixture consumes the unchanged caller-provided Knight GLB
and original CC0 license. It builds nothing.

The game opts into the existing 352-byte oriented-IK service. Every Tick reads
both leg slots. Mode 1 writes both complete committed controls back; mode 2
stages both writes and then throws. The Python fixture checks actual controls,
complete native saved-state rollback and warm/cold continuation. The metric
profile stays in the authored native rig and uses no new managed ABI.

This is controlled pose evidence on an imported character. It does not choose
foot contacts, infer gait phases, ground feet automatically or render a frame.
