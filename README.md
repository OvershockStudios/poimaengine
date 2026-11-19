# Poima Engine

**An open-source, agent-native 3D game engine.**

Describe the game you want to play. Codex or Claude Code can edit the world,
playtest it, and inspect what happened through Poima. Open the same project in
the desktop editor whenever you want to see or change it yourself.

## Winter Village

[![Watch the Winter Village showcase](https://i.ytimg.com/vi/OjqWUCXsNX8/maxresdefault.jpg)](https://www.youtube.com/watch?v=OjqWUCXsNX8)

A one-minute gameplay showcase built and playtested with Codex in Poima.
[Watch on YouTube](https://www.youtube.com/watch?v=OjqWUCXsNX8)

## Get started

1. [Build and install Poima](docs/BUILD.md).
2. Run `poima project create MyGame --name "My Game"`.
3. Open `MyGame` in Codex or Claude Code and [connect Poima](docs/AGENT_CLIENTS.md#connect-a-project).

Start with an idea and attach any sketches or references. Ask the agent to help
you decide what the player does before building. [Plan your first game](docs/GAME_BRIEF.md)

> I want to make a mystery game in a snowy village. Ask me a few questions at a
> time and help me figure out what the player should do.

## Current status

Poima is in pre-alpha. Graphics and the desktop editor target Windows, with
headless authoring and simulation on Linux. Four small agent-built games passed
independent gameplay, save continuation, and standalone Windows checks.
[Implementation status](docs/IMPLEMENTATION_STATUS.md) · [Alpha gates](docs/ALPHA_ROADMAP.md)

[Editor](docs/DESKTOP_EDITOR.md) · [Examples](#examples) · [Documentation](docs/PLAYBOOKS.md) · [Roadmap](docs/ROADMAP.md) · [Changelog](CHANGELOG.md)

### Examples

[Collection Room](examples/collection-game) · [Locomotion Yard](examples/locomotion-yard) · [Physical Character Yard](examples/physical-character-yard)

## Credits and license

Created and architected by Divesh Gupta ([Legendile7](https://github.com/Legendile7)),
publishing as Overshock Studios. Implemented with Codex, including GPT-6-Astra.

[Apache-2.0](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md) · [Branding notice](TRADEMARKS.md)
