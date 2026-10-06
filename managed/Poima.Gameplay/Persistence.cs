// SPDX-License-Identifier: Apache-2.0
namespace Poima;

/// <summary>Opt in to literal, stable-ID metadata for every global state field.
/// Revision identifies the authored save schema, not the service ABI.
/// This annotation alone does not authorize a save upgrade.</summary>
[AttributeUsage(AttributeTargets.Struct,Inherited=false)]
public sealed class GameplayPersistenceAttribute(int revision) : Attribute
{ public int Revision { get; }=revision; }
