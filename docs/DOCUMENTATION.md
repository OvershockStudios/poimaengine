# Documentation and agent workflow standard

Documentation is a release requirement for every supported engine capability
and official package. The goal is a complete developer manual and precise
reference, with working task guides for both people and agents. Page count is
not the measure: a developer must be able to build, inspect, debug and deliver
the documented result without guessing hidden setup or unsupported commands.

## Human manual

Provide concepts, project setup, authoring workflows, code examples, visual
guides and troubleshooting. Cover the relationships between systems: animation
and movement, material and lighting, weather and surfaces, physics and queries,
gameplay and saves, and rendering and performance. Clearly distinguish core
services from optional packages and name supported targets and limits.

Quickstarts lead to an actual playable, exported result. Examples use licensed
or original content with reproducible setup. Screenshots and captures identify
the workflow they show and are refreshed when that workflow changes.

## API and package reference

Generate reference facts from authoritative schemas and compiled APIs where
possible: fields, types, defaults, units, constraints, capability requirements,
errors and version compatibility. Add authored explanation for ownership,
lifetime, ordering, migration and performance behavior that a type signature
cannot explain.

Document native and C# interfaces together with the command API. Official
packages need installation, locked dependencies, authoring/runtime/cooking/save
integration, examples, troubleshooting and license notices. Versioned reference
must match the artifacts and installed package set it describes.

## Task playbooks

Playbooks are focused Markdown guides for a result, such as importing a
character, constructing an FPS encounter, configuring a scoped weapon, building
terrain or investigating a frame hitch. Each includes:

- The intended result and supported capability/package prerequisites.
- A minimal successful sequence with commands or compiled code.
- Checkpoints using semantic state and visual/audio observations as appropriate.
- Common errors, conflict/retry handling and safe recovery.
- Acceptance checks and links to deeper reference and profiling guidance.

Recipes must use operations the engine actually supplies. A future capability
matrix is planning documentation, not a runnable playbook. Long tasks compose
small guides rather than duplicating a complete manual in every prompt.

Each playbook names its tested engine/package versions, required build options,
platform and toolchain. State whether a path is an input, a new output or a
directory that can be reused. Show expected observations at meaningful steps,
not just the command to run. Separate a request's acknowledgement from its
completed result, and explain what survives reload, restart and export.

Provide focused paths for different readers: a concept guide for learning, a
recipe for completing a task, and a reference for resolving an exact field or
error. Agents should retrieve the relevant task and schema section as needed;
ordinary edits must not require loading the complete manual into context.

The [task index](PLAYBOOKS.md) lists runnable examples and focused guides with
their build prerequisites and completion checks. Start there when choosing a
workflow; discover the actual installed capabilities before executing it.

The runnable [guarded scene edit](../examples/guarded-authoring) is an initial
playbook: discovery, preview, atomic changes, conflict reconciliation, receipt
replay and fresh-owner persistence. [Collection Room](../examples/collection-game)
provides a longer compiled-game example with input and save continuation.

## Codex and Claude skills

Skills provide compact entry points to tested playbooks and live capability
discovery. They explain how to choose a workflow, obtain focused schemas,
perform guarded edits, observe results and verify completion. They link to
authoritative reference instead of copying a large, stale command catalogue.

Qualify skills through actual agent runs, including failure recovery and human
edits in the same session. Installed package discovery determines what is
available; a skill does not supply missing engine features or imply that an
unsupported platform is qualified.

## Release checks

Run documented quickstarts/examples against the matching engine build and
supported package configurations. Validate links and generated-reference drift;
test migrations, errors and exported-game setup where those are documented.
Keep observed results and limitations available without repeating development
boilerplate on every page. A broken core tutorial, missing supported API
reference or undocumented required setup blocks the corresponding release gate.

Track coverage against supported capabilities and public APIs. For each, record
its manual/reference entry, task example, troubleshooting and verification
coverage; an index entry alone does not establish completeness. Examples and
reference must also explain interactions between systems, not just isolated
features. Publish the support/version matrix with the matching release so a
reader can distinguish documented support from a planned capability.
