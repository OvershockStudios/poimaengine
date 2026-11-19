# Plan your first game in chat

<!-- SPDX-License-Identifier: Apache-2.0 -->

Open a Poima project in your coding agent. Describe the game you want to make
in your own words. Add sketches, screenshots or references if they help explain
how it should look or feel. Say which part of an image matters to you, such as
its colors, camera, level layout or atmosphere. Use the agent’s planning mode
when available.

New projects made with `poima project create` include these instructions for
the agent. You can change them in the project’s `AGENTS.md`.

> Help me plan this game first. Ask a few multiple-choice questions at a time,
> adapt the next questions to my answers, and show me the game brief before you
> build anything. I’d like to be able to leave some choices to you.

## Ask in rounds

Start with what the player will do and what the creator wants it to feel like.
Ask one to three questions per round. Offer concrete choices, explain the
consequence of each in one line, and accept a free-text answer. Include a
delegate choice when the agent can make a useful default. Use the host’s choice
controls where available. A short numbered menu works in plain chat.

Use each answer to decide which questions still matter. For a snowy village
idea, the first question might ask whether the player explores, solves a
mystery, or survives a night. A mystery answer can prompt a question about clues
or time pressure. A quiet exploration answer can lead to movement, characters,
and how the town reacts. Keep earlier choices visible when a later choice
changes them. Ask about images instead of assuming the creator wants every
visible element.

Cover decisions that change the game. Typical topics include the player’s
perspective and controls, repeated actions, challenge and failure, progress and
saves, interaction and characters, world style and sound, accessibility, and
where the game should run. Ask about detailed features when they matter to a
chosen direction. Make sensible defaults for details the creator delegates.

## Agree on the brief

Summarize what a player will experience, what counts as a complete first
playable build, and what the creator has delegated. Call out remaining
decisions that could materially change the game. Verify actual project and
engine capabilities before promising a feature. Offer a supported alternative
or discuss the work needed if an important capability is missing.

Get the creator’s agreement before starting implementation. Keep the accepted
brief available during playtesting so the agent can compare the result with the
intended experience. After play, ask focused follow-up questions where the
creator’s feedback changes a decision. The same project can be opened in the
editor whenever direct editing would help.

The interview can be run by a coding agent today. Interactive question cards,
shared review and an integrated project launcher are planned interfaces for the
same workflow.
