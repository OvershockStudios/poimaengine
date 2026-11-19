<h1><a href="https://poimaengine.com/"><img src="docs/branding/poima-wordmark.svg" alt="Poima Engine" width="208" height="77"></a></h1>

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

Describe your idea and attach any sketches or references. New projects guide
the agent to ask a few questions at a time before building. [Plan your first game](docs/GAME_BRIEF.md)

> I want to make a mystery game in a snowy village where the lamps go out every night.

## Current status

Poima is in pre-alpha. The CLI supports headless authoring and playtesting on
Windows and Linux. The Vulkan player and desktop editor run on Windows.
Four small agent-built games passed independent gameplay, save continuation,
and standalone Windows checks.
[Implementation status](docs/IMPLEMENTATION_STATUS.md) · [Alpha gates](docs/ALPHA_ROADMAP.md)

[Editor](docs/DESKTOP_EDITOR.md) · [Examples](#examples) · [Documentation](docs/PLAYBOOKS.md) · [Roadmap](docs/ROADMAP.md) · [Changelog](CHANGELOG.md)

### Examples

[Collection Room](examples/collection-game) · [Locomotion Yard](examples/locomotion-yard) · [Physical Character Yard](examples/physical-character-yard)

## Credits and license

Created and architected by Divesh Gupta ([Legendile7](https://github.com/Legendile7)),
publishing as Overshock Studios. Built with GPT-6 Astra, GPT-6 Sol, GPT-6.2 Astra,
GPT-6.2 Sol, and Claude Opus 5.5.

[Apache-2.0](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md) · [Branding notice](TRADEMARKS.md)
