// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Numerics;
namespace Poima.Examples;
public struct DoorState
{
    public EntityId Player,Camera,Door;
    public double OpenX,UseDistance;
    public int Open,Activations;
}
[GameModule("poima.example.sliding-door")]
public sealed class DoorGame : Game<DoorState>
{
    public override void Initialize(ref DoorState state)
    {
        state.Player=new(0,100);state.Camera=new(0,101);state.Door=new(0,2);
        state.OpenX=2.2;state.UseDistance=3;
    }
    public override void Tick(ref DoorState state,GameContext context)
    {
        if(!context.Pressed(state.Player,GameAction.Use))return;
        var camera=context.Get(state.Camera).Transform;
        var hit=context.Raycast(camera.Position,camera.Forward,state.UseDistance,[state.Player]);
        if(hit is null || hit.Value.Entity!=state.Door)return;
        var position=context.Get(state.Door).Transform.Position;
        state.Open=state.Open==0 ? 1 : 0;
        context.MoveKinematic(state.Door,new(state.Open!=0 ? state.OpenX : 0,position.Y,position.Z),Quaternion.Identity,120);
        ++state.Activations;
    }
}
