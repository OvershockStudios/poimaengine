# Compiled collection wire fixture

<!-- SPDX-License-Identifier: Apache-2.0 -->

`Poima.Tests.CollectionWireGame` exercises the generated native component codecs
for all five supported array element kinds. Each buffer has capacity two; five
48-byte buffers and one 16-byte scalar occupy 256 native wire bytes. CLR struct
layout does not define the wire format.

The stable component and field IDs appear in the original C# source. The
[workflow guide](../../docs/COMPONENT_GAME_WORKFLOW.md#bounded-collection-profile)
publishes the module with matching SDK/generators, creates a typed project,
adopts it, exports and checks both source and actual readonly game services.

Mode 0 queries and reads with no component writes. Mode 1 publishes known
integer extrema, lossless `long` extrema, finite floating values and valid entity
handles on the first entity, increments its revision and proves staged writes
remain invisible inside Tick. Later ticks check the previous publication.
Mode 2 queues a duplicate target; Mode 3 stages an unresolved entity handle.
Both failures must preserve the selected state. These are qualification modes,
not recommended game logic.

This fixture covers capacity-two native component buffers and exact-image
saves. Arbitrary managed arrays, nested buffers, global-state collections,
capacity migration, rendering and representative performance are separate
scopes. [Collection contract](../../docs/COMPONENT_COLLECTIONS.md).
