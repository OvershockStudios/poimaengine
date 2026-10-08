# Agent-authored escape-room fixture

These are the C# source, authored world and input route from one external Codex
CLI exercise. The original C# source is retained exactly. The world keeps its authored data
and revision with receipt history removed; the project
file uses a relative SDK reference for this checkout, and manifest assembly/save
paths are relative. This is recorded evidence with primitive geometry.

The agent authored three collectible keys, a locked exit condition, logical HUD,
pause/resume controls and checkpoint saves. It built the C# module, corrected
invalid UI input and low camera placement, then played the room through native
controller inputs. An independent fresh-runtime replay checked the locked exit,
all three pickups, completion, exact checkpoint restoration and completion again.

[Recorded results and limits](../../m2-agent-game.json). This checkpoint covers
headless gameplay and logical UI, with no graphical or physical-input claim.
The source is the measured generated game, including basic save-status handling
and per-interaction allocations; the Collection Room remains the documented
SDK sample.
