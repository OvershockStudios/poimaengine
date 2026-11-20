<h1><a href="https://poimaengine.com/"><img src="docs/branding/poima-wordmark.svg" alt="Poima Engine" width="208" height="77"></a></h1>

**An open-source 3D game engine and agent-native creation framework.**

Poima is designed to help anyone make a game by describing it, playing a
build, and directing the next change. The creation framework helps turn an
idea into a game brief and gives agents a workflow for building, playtesting,
and revising it. The native engine runs the game and exposes what happened
during the playtest. The same projects open in the desktop editor.
[Roadmap](docs/ROADMAP.md#product-milestones)

## Winter Village

[![Watch the Winter Village showcase](https://i.ytimg.com/vi/JH9HWhj7jpE/maxresdefault.jpg)](https://www.youtube.com/watch?v=JH9HWhj7jpE)

A one-minute gameplay showcase built and playtested with Codex in Poima.
[Watch on YouTube](https://www.youtube.com/watch?v=JH9HWhj7jpE)

## Get started

1. [Set up Poima for Windows](docs/DESKTOP_EDITOR.md#build-and-launch) and open `Poima.Editor.exe`.
2. Name your game and select **Create project**. Choose **Codex**, **Antigravity**, or **Claude Code**. [Agent setup](docs/AGENT_CLIENTS.md)
3. Describe your idea, agree on a short game brief, and press **Play** to try the project.

Describe your idea in your own words. New projects guide the agent to ask a
few questions at a time before building. [Plan your first game](docs/GAME_BRIEF.md)

> I want to make a mystery game in a snowy village where the lamps go out every night.

For terminal or headless use, [install the CLI](docs/BUILD.md), run
`poima project create MyGame`, and [connect an agent](docs/AGENT_CLIENTS.md#connect-a-project).

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
