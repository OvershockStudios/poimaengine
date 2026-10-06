# Compiled UI control fixture

`Poima.Tests.ManagedUiGame` exercises native UI reads/writes from ordinary ticks
and separate control turns. Stable controls use IDs 1–13 (32-digit lowercase
hexadecimal): root, label, Edit, Save, Resume, Pause, Throw, Open modal, modal
panel, Close modal, Invalid edit, disabled button, and Load.

The fixture tests committed reads, merged queued writes, typed global state,
save intents, playback intents, and exceptions. It requires matching native
UI services and runs as either CoreCLR gameplay or a published NativeAOT module.
`NoUiHandlerGame` is a separate CoreCLR-only optional-handler test.

This fixture does not route OS input or render controls. Its Save action is
implemented by compiled game code; UI action strings alone do not save a game.
