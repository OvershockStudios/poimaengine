// SPDX-License-Identifier: Apache-2.0
using Poima;

namespace Poima.Examples;

// Optional per-character data. The silent profile registers this declaration
// but supplies no instances. Cadence belongs to native component/save/rollback
// state; RelayState's existing persistent globals do not change.
[GameplayComponent("c0870000000000000000000000000001")]
public partial struct RelayAudioCadence
{
    [GameplayField("00000000000000000000000000000001",Default="1")] public int Enabled;
    [GameplayField("00000000000000000000000000000002")] public EntityId StepEmitterA;
    [GameplayField("00000000000000000000000000000003")] public EntityId StepEmitterB;
    [GameplayField("00000000000000000000000000000004",Default="0.8",Unit="m")] public double StrideMeters;
    [GameplayField("00000000000000000000000000000005",Default="0.25")] public float Gain;
    [GameplayField("00000000000000000000000000000006")] public int PreviousValid;
    [GameplayField("00000000000000000000000000000007")] public long PreviousTick;
    [GameplayField("00000000000000000000000000000008",Unit="m")] public double PreviousX;
    [GameplayField("00000000000000000000000000000009",Unit="m")] public double PreviousY;
    [GameplayField("0000000000000000000000000000000a",Unit="m")] public double PreviousZ;
    [GameplayField("0000000000000000000000000000000b",Unit="m")] public double TravelMeters;
    [GameplayField("0000000000000000000000000000000c")] public int NextVariation;
    [GameplayField("0000000000000000000000000000000d")] public long StepCount;
    [GameplayField("0000000000000000000000000000000e",Unit="m")] public double LastDisplacement;
    [GameplayField("0000000000000000000000000000000f")] public int Grounded;
    [GameplayField("00000000000000000000000000000010")] public long LastVoice;
    [GameplayField("00000000000000000000000000000011")] public long SampleCount;
}

internal static class RelayAudio
{
    // These emitters are permanent authored nodes. A collectible's despawn
    // retires its own voices, so pickup feedback must not live on that prop.
    internal static readonly EntityId Pickup=new(0,610),Denied=new(0,611),
        Arrival=new(0,612),Win=new(0,613);
    const int MaxStepsPerTick=2;
    const double MaxPlanarSpeed=8; // Sample controllers are authored at 4 m/s.

    static void Check(bool value,string message)
    {
        if(!value)throw new InvalidOperationException(message);
    }

    internal static void Cue(GameContext context,EntityId emitter)
    {
        // No source graph or queued birth is inferred. Missing permanent nodes
        // are the supported silent profile; present nodes need valid emitters.
        if(context.IsAlive(emitter))context.PlaySound(emitter);
    }

    static void Validate(in RelayAudioCadence cadence)
    {
        Check(cadence.Enabled is 0 or 1 && cadence.PreviousValid is 0 or 1 &&
              cadence.Grounded is 0 or 1 && cadence.NextVariation is 0 or 1,
              "Relay audio cadence flags must be 0 or 1.");
        Check(double.IsFinite(cadence.StrideMeters) && cadence.StrideMeters is >=.05 and <=5 &&
              float.IsFinite(cadence.Gain) && cadence.Gain is >=0 and <=4,
              "Relay audio stride/gain is outside the sample's finite bounds.");
        Check(cadence.PreviousTick is >=0 and <=9007199254740991L && cadence.StepCount>=0 &&
              cadence.SampleCount>=0 && cadence.LastVoice>=0,
              "Relay audio cadence chronology/counters must be nonnegative.");
        Check(double.IsFinite(cadence.PreviousX) && double.IsFinite(cadence.PreviousY) &&
              double.IsFinite(cadence.PreviousZ) && double.IsFinite(cadence.LastDisplacement) &&
              cadence.LastDisplacement>=0 && double.IsFinite(cadence.TravelMeters) &&
              cadence.TravelMeters>=0 && cadence.TravelMeters<cadence.StrideMeters,
              "Relay audio cadence position/phase must remain finite and bounded.");
    }

    internal static void Steps(GameContext context,EntityId character)
    {
        // TryGet returns false for an existing entity without this registered
        // component. Declaration registration is supplied by the full manifest.
        if(!context.TryGet<RelayAudioCadence>(character,out var cadence))return;
        Validate(in cadence);
        var entity=context.Get(character);
        Check(entity.Motion==BodyMotion.Character,"Relay cadence needs a native character root.");
        var position=entity.Transform.Position;
        Check(double.IsFinite(position.X) && double.IsFinite(position.Y) && double.IsFinite(position.Z) &&
              double.IsFinite(entity.Velocity.Y) && double.IsFinite(context.DeltaTime) &&
              context.DeltaTime>0,"Relay audio requires finite native motion and a positive fixed tick.");
        long tick=checked((long)context.Tick);
        bool consecutive=cadence.PreviousValid!=0 && cadence.PreviousTick+1==tick;
        double dx=position.X-cadence.PreviousX,dz=position.Z-cadence.PreviousZ;
        double displacement=consecutive?Math.Sqrt(dx*dx+dz*dz):0;
        Check(double.IsFinite(displacement),"Relay audio displacement overflowed.");
        bool previouslyGrounded=cadence.Grounded!=0;
        bool grounded=false;
        if(cadence.Enabled!=0)
        {
            Check(cadence.StepEmitterA!=default && cadence.StepEmitterB!=default &&
                  context.IsAlive(cadence.StepEmitterA) && context.IsAlive(cadence.StepEmitterB),
                  "Enabled relay cadence needs two live permanent emitters.");
            Span<EntityId> ignore=stackalloc EntityId[1];ignore[0]=character;
            var hit=context.Raycast(new(position.X,position.Y+.08,position.Z),new(0,-1,0),.15,ignore);
            // This is a short support ray for the sample's flat floor, not a
            // new CharacterController grounding API. Vertical motion, missing
            // support and a steep/side face cannot advance footstep phase.
            grounded=hit is { Normal: { } normal } && normal.Y>=.65 && Math.Abs(entity.Velocity.Y)<=.25;
        }
        cadence.LastDisplacement=displacement;
        bool safeTravel=consecutive && grounded && previouslyGrounded &&
            Math.Abs(position.Y-cadence.PreviousY)<=.025 &&
            displacement<=MaxPlanarSpeed*context.DeltaTime+.001;
        if(cadence.Enabled==0 || !safeTravel)
            cadence.TravelMeters=0;
        else if(displacement>1e-6)
        {
            double travel=cadence.TravelMeters+displacement;
            int steps=(int)Math.Min(MaxStepsPerTick,Math.Floor(travel/cadence.StrideMeters));
            for(int index=0;index<steps;++index)
            {
                var emitter=cadence.NextVariation==0?cadence.StepEmitterA:cadence.StepEmitterB;
                cadence.LastVoice=context.PlaySound(emitter,cadence.Gain);
                cadence.NextVariation=1-cadence.NextVariation;
                cadence.StepCount=checked(cadence.StepCount+1);
            }
            // Drop any excess at the per-tick safety ceiling. It must not turn
            // into a delayed burst when the character subsequently stops.
            cadence.TravelMeters=Math.Clamp(travel-steps*cadence.StrideMeters,0,
                                            Math.BitDecrement(cadence.StrideMeters));
        }
        cadence.Grounded=grounded?1:0;
        cadence.PreviousX=position.X;cadence.PreviousY=position.Y;cadence.PreviousZ=position.Z;
        cadence.PreviousTick=tick;cadence.PreviousValid=cadence.Enabled;
        cadence.SampleCount=checked(cadence.SampleCount+1);
        context.Set(character,in cadence);
    }
}
