# Bounded component collections (development)

Collection fields store ordered lists of scalar values or entity handles in native-owned components. Linux native and compiled CoreCLR development tests cover ordered mutation, failed-tick rollback, spawned references and fresh-process save continuation. The [Linux evidence](evidence/m2-component-collections.json) remains a separate historical record.

Windows CoreCLR is qualified against the final 0.0.51 binary: the existing compiled inventory fixture passes 151 RPCs and three clean owner-process exits. Its [separate evidence record](evidence/m2-component-collections-windows.json) identifies the sources, artifacts and checks.

Windows Native AOT is qualified in 0.0.52 for the same capacity-four entity/int32 component buffers. The unchanged 151-RPC contract passes with byte-exact failed-batch rollback and two fresh-process save continuations. A separate exported and relocated native player runs seven scripted ticks; the headless save contract also passes through that bundled runtime and artifact using its own fixture world. Those save checks do not demonstrate in-player inventory or save UX. [Native artifact and bundle evidence](evidence/m2-component-collections-native-windows.json).

These results establish bounded component behavior, not all supported scalar kinds, a complete inventory game, clean-machine deployment or game-scale performance. Global-state collections, nested buffers, arbitrary managed arrays and capacity migration remain unqualified or unsupported as described below.

## Declare and use a buffer

Declare the buffer and its component in the same game assembly, with the component source generator enabled:

```csharp
using Poima;

[GameplayBuffer(typeof(EntityId), 16)]
public partial struct InventorySlots {}

[GameplayComponent("77269d0f7c53478fabbe02a17bcf9451")]
public partial struct Inventory
{
    [GameplayField("036e8748ac714d8f817af4f74f587cb1")]
    public InventorySlots Items;
}
```

These IDs illustrate the format; generate fresh stable IDs for your own declarations. Keep them across renames. Buffer declarations must be empty, public, mutable, nongeneric partial structs. Supported element types are `int`, `long`, `float`, `double` and `EntityId`. Nested buffers, managed objects and buffers declared in referenced assemblies are unsupported.

Inside a gameplay callback:

```csharp
var inventory = context.Get<Inventory>(owner);
if (inventory.Items.TryAdd(item))
    context.Set(owner, in inventory);
```

`Count` reports the logical length; `Capacity` is fixed by the declaration. The indexer reads or replaces an existing item. `TryAdd` returns false when full. `RemoveAt` shifts subsequent items left, preserving order; `Clear` empties the buffer. Invalid indices throw. Nonfinite floating values are rejected and negative zero is normalized. Duplicate values and unset entity handles are permitted.

`Get` returns a copy. Mutating that copy does not edit the world until `Set` queues the whole component. Reads within the same callback continue to see published state. Existing transaction and batch rollback rules apply to the complete component, including its length and item order.

An entity handle in an active slot is a strong runtime reference. Despawning its target requires removing or clearing all surviving references in that same atomic tick. Inactive capacity slots contain no references. Template data and its reference rules remain separate from live instances.

## Capacity and representation

A component still permits at most 32 fields and 512 bytes of wire data. A scalar consumes 16 bytes. Each buffer consumes a 16-byte length header plus 16 bytes per capacity slot, including unused slots. Capacities are 1–31, subject to the aggregate component budget. A 16-item entity buffer consumes 272 bytes; a 31-item buffer consumes the full 512-byte allowance.

Buffers start empty. Schema defaults cannot contain items; authored component values and spawn-template values can. The generated structs remain unmanaged, but their CLR memory layout is not the wire format. Generated codecs explicitly encode lengths, scalar cells and canonical padding. Global `Game<TState>` collections are not supported by this component feature.

## Agent authoring and compatibility

`world.describe` exposes schema version 2 for components containing collections. A collection field has `kind:"array"`, an `element_kind`, `capacity`, and `default:[]`. Values use ordinary JSON arrays under stable field IDs. Int64 elements remain canonical decimal strings; entity handles remain 32 lowercase hexadecimal digits. A complete component edit supplies all fields and the usual revision guards.

Pure scalar components retain schema version 1 and their existing fingerprints. Collection schemas include capacity and element kind in their fingerprint. Exact restore and reload still require compatible declared schemas; changing capacity is not automatic resizing. The explicit scalar save-upgrade mapper rejects collection mappings. It must never silently truncate items.

Native artifacts require `component_collections_v1`; runtime selection and export check that capability. Call ABI 1 and the services ABI 7, 176-byte baseline remain unchanged. A runtime without collection support must reject the artifact. Use a matching SDK, generator and bridge for new collection games.
