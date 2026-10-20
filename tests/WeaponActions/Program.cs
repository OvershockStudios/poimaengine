// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using Poima.Fps;

static class Program
{
    private static int checks;
    private static void Require(bool value,string message)
    {++checks;if(!value)throw new Exception(message);}
    private static bool Has(WeaponEvents value,WeaponEvents bit)=>(value&bit)!=0;
    private static WeaponDefinition Definition(WeaponFireMode mode=WeaponFireMode.SemiAutomatic,int capacity=4,int interval=3)
        =>new(capacity,interval,2,3,2,2,mode,3);
    private static WeaponState Ready(int magazine=2,int chamber=1,int reserve=3)
        =>new(magazine,chamber,reserve,WeaponPhase.Ready,0,0,0,false,0);
    private static WeaponStep Tick(WeaponDefinition definition,ref WeaponState state,WeaponInput input=default)
    {var result=Weapons.Step(in definition,in state,in input);state=result.State;return result;}
    private static long Ammo(WeaponState state)=>(long)state.Magazine+state.Chamber+state.Reserve;
    private static void Events(WeaponStep step,WeaponEvents expected,string message)
        =>Require(step.Events==expected,message);
    private static void Phase(WeaponState state,WeaponPhase expected,int remaining,string message)
        =>Require(state.Phase==expected&&state.TicksRemaining==remaining,message);
    private static void Reject(WeaponDefinition definition,WeaponState state,WeaponInput input=default)
    {
        var oldDefinition=definition;var oldState=state;var oldInput=input;
        byte[] original=JsonSerializer.SerializeToUtf8Bytes(state);bool rejected=false;
        try{Weapons.Step(in definition,in state,in input);}
        catch(ArgumentException){rejected=true;}
        catch(InvalidOperationException){rejected=true;}
        catch(ArithmeticException){rejected=true;}
        Require(rejected,"Invalid definition/state or accepted-shot overflow must reject.");
        Require(definition==oldDefinition&&state==oldState&&input==oldInput&&JsonSerializer.SerializeToUtf8Bytes(state).SequenceEqual(original),"Rejection cannot mutate any caller-owned input field.");
    }
    private static void ContractAndCadence()
    {
        Require(Weapons.MaxAmmo==1_000_000&&Weapons.MaxTicks==1_000_000,"Public finite domains changed.");
        WeaponEvents[] flags=[WeaponEvents.Shot,WeaponEvents.DryFire,WeaponEvents.ReloadStarted,WeaponEvents.MagazineEjected,WeaponEvents.MagazineInserted,WeaponEvents.Chambered,WeaponEvents.ReloadCancelled,WeaponEvents.Holstered,WeaponEvents.EquipStarted,WeaponEvents.Equipped];
        Require(WeaponEvents.None==0,"None is the empty event set.");
        for(int i=0;i<flags.Length;++i)Require((int)flags[i]==1<<i,"Events must be independent stable flags.");
        // Explicit input/shot tables distinguish held edges, lost busy edges and
        // cadence. The oracle lists accepted ticks, not another state machine.
        var semi=Definition(capacity:10);var state=Ready(10,1,0);var shots=new List<int>();
        bool[] held=[true,true,true,true,false,true,false,true,false,true];
        for(int i=0;i<held.Length;++i)if(Has(Tick(semi,ref state,new(TriggerHeld:held[i])).Events,WeaponEvents.Shot))shots.Add(i+1);
        Require(shots.SequenceEqual(new[]{1,6,10}),"Semi-auto accepts rising edges only; a cooldown-rejected edge is not queued.");
        var automatic=Definition(WeaponFireMode.Automatic,10);state=Ready(10,1,0);shots.Clear();
        for(int tick=1;tick<=10;++tick)if(Has(Tick(automatic,ref state,new(TriggerHeld:true)).Events,WeaponEvents.Shot))shots.Add(tick);
        Require(shots.SequenceEqual(new[]{1,4,7,10}),"Auto cadence must be exactly every three ticks, including cooldown1->0.");
        var burst=Definition(WeaponFireMode.Burst,10);state=Ready(10,1,0);shots.Clear();
        bool[] burstHeld=[true,false,false,false,true,true,true,true,true,true,false,true,false,false,false,false,false,false];
        for(int i=0;i<burstHeld.Length;++i)if(Has(Tick(burst,ref state,new(TriggerHeld:burstHeld[i])).Events,WeaponEvents.Shot))shots.Add(i+1);
        Require(shots.SequenceEqual(new[]{1,4,7,12,15,18}),"Burst continues after release, does not queue an edge during burst, and restarts on a fresh later edge.");
        var fast=Definition(WeaponFireMode.Automatic,4,1);state=Ready(4,1,0);
        for(int i=0;i<5;++i)Events(Tick(fast,ref state,new(TriggerHeld:true)),WeaponEvents.Shot,"Interval1 permits a shot each ready tick.");
        Require(state.Magazine==0&&state.Chamber==0&&state.AcceptedShots==5,"Closed-bolt feed consumes exactly one round per accepted shot.");
    }
    private static void ReloadStages()
    {
        var definition=Definition();var state=Ready();long total=Ammo(state);
        Events(Tick(definition,ref state,new(ReloadPressed:true)),WeaponEvents.ReloadStarted,"Partial reload starts without advancing.");Phase(state,WeaponPhase.ReloadEject,2,"Full eject timer required.");
        Events(Tick(definition,ref state),WeaponEvents.None,"First eject tick has no inventory effect.");Require(state.Magazine==2&&state.Reserve==3,"Magazine remains until eject completion.");
        Events(Tick(definition,ref state),WeaponEvents.MagazineEjected,"Eject completes once.");Phase(state,WeaponPhase.ReloadInsert,3,"Insert starts at full duration.");Require(state.Magazine==0&&state.Chamber==1&&state.Reserve==5,"Partial magazine returns to reserve while chamber survives.");
        for(int i=0;i<2;++i)Events(Tick(definition,ref state),WeaponEvents.None,"Insert does not transfer early.");
        Events(Tick(definition,ref state),WeaponEvents.MagazineInserted,"Insertion is one committed transfer.");Phase(state,WeaponPhase.Ready,0,"Preserved chamber avoids an extra chamber stage.");Require(state.Magazine==4&&state.Chamber==1&&state.Reserve==1&&Ammo(state)==total,"Tactical reload permits full magazine plus preserved chamber without creating ammo.");
        // Empty reload has a distinct final stage and ends at capacity rounds,
        // not capacity+1. Only the chamber operation removes a magazine round.
        state=Ready(0,0,6);Tick(definition,ref state,new(ReloadPressed:true));
        for(int i=0;i<2;++i)Tick(definition,ref state);
        for(int i=0;i<3;++i)Tick(definition,ref state);
        Phase(state,WeaponPhase.Chambering,2,"Empty reload must chamber after insertion.");Require(state.Magazine==4&&state.Chamber==0&&state.Reserve==2,"Insertion cannot secretly chamber.");
        Tick(definition,ref state);Events(Tick(definition,ref state),WeaponEvents.Chambered,"Chamber completes exactly once.");Require(state.Magazine==3&&state.Chamber==1&&state.Reserve==2,"Empty reload retains complete inventory.");
        state=Ready(0,0,2);Tick(definition,ref state,new(ReloadPressed:true));
        for(int i=0;i<7;++i)Tick(definition,ref state);
        Require(state.Phase==WeaponPhase.Ready&&state.Magazine==1&&state.Chamber==1&&state.Reserve==0,"Insufficient reserve yields a partial magazine and chamber without fabricated ammo.");
        state=Ready(2,0,0);Events(Tick(definition,ref state,new(ReloadPressed:true)),WeaponEvents.ReloadStarted,"Magazine-only reload starts chambering.");Phase(state,WeaponPhase.Chambering,2,"No reserve requires no eject/insert detour.");
        Tick(definition,ref state);Tick(definition,ref state);Require(state.Magazine==1&&state.Chamber==1,"Manual chamber consumes one existing magazine round.");
        state=Ready(1,0,2);Tick(definition,ref state,new(ReloadPressed:true));Phase(state,WeaponPhase.ReloadEject,2,"Eligible reserve refill precedes chamber-only fallback.");
    }
    private static void CancelCommittedStages()
    {
        var definition=Definition();var before=Ready();var state=before;
        Tick(definition,ref state,new(ReloadPressed:true));
        Events(Tick(definition,ref state,new(TriggerHeld:true,ReloadPressed:true,CancelReload:true)),WeaponEvents.ReloadCancelled,"Cancel wins over new reload/fire during reload.");
        Require(state.Magazine==before.Magazine&&state.Chamber==before.Chamber&&state.Reserve==before.Reserve&&state.TriggerHeld,"Pre-eject cancellation keeps every round and remembers held input.");Phase(state,WeaponPhase.Ready,0,"Cancellation is ready immediately.");
        state=before;Tick(definition,ref state,new(ReloadPressed:true));Tick(definition,ref state);Tick(definition,ref state);
        Events(Tick(definition,ref state,new(CancelReload:true)),WeaponEvents.ReloadCancelled,"Post-eject cancellation is permitted.");Require(state.Magazine==0&&state.Chamber==1&&state.Reserve==5,"Cancellation cannot undo an already committed eject.");
        state=Ready(0,0,6);Tick(definition,ref state,new(ReloadPressed:true));for(int i=0;i<5;++i)Tick(definition,ref state);
        Events(Tick(definition,ref state,new(CancelReload:true)),WeaponEvents.ReloadCancelled,"Chambering can be interrupted.");Require(state.Magazine==4&&state.Chamber==0&&state.Reserve==2,"Post-insert cancellation keeps inserted inventory and does not fabricate a chamber round.");
        Tick(definition,ref state,new(ReloadPressed:true));Phase(state,WeaponPhase.Chambering,2,"Restart chooses chamber-only stage when magazine is full.");
        state=Ready()with{Cooldown=2};Tick(definition,ref state,new(ReloadPressed:true));
        var cancelled=Tick(definition,ref state,new(CancelReload:true));Require(cancelled.State.Cooldown==0,"Cooldown still ages before cancel priority.");
        state=Ready();Events(Tick(definition,ref state,new(TriggerHeld:true,CancelReload:true)),WeaponEvents.Shot,"Cancel outside reload cannot suppress a valid shot.");
    }
    private static void HolsterEquipAndEdges()
    {
        var definition=Definition();WeaponPhase[] phases=[WeaponPhase.Ready,WeaponPhase.Equipping,WeaponPhase.ReloadEject,WeaponPhase.ReloadInsert,WeaponPhase.Chambering];
        foreach(var phase in phases)
        {
            var state=Ready()with{Phase=phase,TicksRemaining=phase==WeaponPhase.Ready?0:1,Cooldown=2,Magazine=phase==WeaponPhase.ReloadInsert?0:2,Chamber=phase==WeaponPhase.Chambering?0:1};long ammo=Ammo(state);
            var step=Tick(definition,ref state,new(TriggerHeld:true,ReloadPressed:true,CancelReload:true,Holster:true,Equip:true));
            var expected=WeaponEvents.Holstered|(phase is WeaponPhase.ReloadEject or WeaponPhase.ReloadInsert or WeaponPhase.Chambering?WeaponEvents.ReloadCancelled:WeaponEvents.None);
            Events(step,expected,"Holster must win before phase completion/cancel/equip/reload/fire.");Phase(state,WeaponPhase.Holstered,0,"Holster clears timer.");Require(Ammo(state)==ammo&&state.Cooldown==1&&state.TriggerHeld&&state.BurstRemaining==0,"Holster preserves inventory and already-aged cooldown.");
        }
        var burst=Definition(WeaponFireMode.Burst);var active=Ready()with{BurstRemaining=2};Tick(burst,ref active,new(Holster:true));Require(active.BurstRemaining==0,"Holster cancels pending burst rounds without firing them.");
        var held=Ready()with{Phase=WeaponPhase.Holstered,TriggerHeld=true};Events(Tick(definition,ref held,new(TriggerHeld:true,Holster:true,Equip:true)),WeaponEvents.None,"Repeated holster has no duplicate event or equip.");
        Events(Tick(definition,ref held,new(TriggerHeld:true,Equip:true)),WeaponEvents.EquipStarted,"Explicit equip starts once.");Phase(held,WeaponPhase.Equipping,2,"Equip start must not decrement itself.");
        Events(Tick(definition,ref held,new(TriggerHeld:true,Equip:true)),WeaponEvents.None,"Busy repeated equip does not restart timer.");
        Events(Tick(definition,ref held,new(TriggerHeld:true)),WeaponEvents.Equipped,"Held semi-auto edge consumed while busy cannot fire on equip completion.");
        Tick(definition,ref held);Events(Tick(definition,ref held,new(TriggerHeld:true)),WeaponEvents.Shot,"Fresh edge after equip fires.");
        held=Ready()with{Phase=WeaponPhase.Equipping,TicksRemaining=2};Tick(definition,ref held,new(TriggerHeld:true));Events(Tick(definition,ref held,new(TriggerHeld:true)),WeaponEvents.Equipped,"Busy input updates held state even if it cannot fire.");
        held=Ready(2,0,0)with{Phase=WeaponPhase.Chambering,TicksRemaining=2};Tick(definition,ref held,new(TriggerHeld:true));Events(Tick(definition,ref held,new(TriggerHeld:true)),WeaponEvents.Chambered,"Semi trigger pressed during chambering is not deferred to completion.");
        Tick(definition,ref held);Events(Tick(definition,ref held,new(TriggerHeld:true)),WeaponEvents.Shot,"A later fresh edge after chamber completion is eligible.");
    }
    private static void CompletionAndDryFire()
    {
        var automatic=Definition(WeaponFireMode.Automatic);var state=Ready()with{Phase=WeaponPhase.Equipping,TicksRemaining=1};
        Events(Tick(automatic,ref state,new(TriggerHeld:true)),WeaponEvents.Equipped|WeaponEvents.Shot,"Equip completion can fire in the same tick.");
        state=Ready(0,1,2)with{Phase=WeaponPhase.ReloadInsert,TicksRemaining=1};
        Events(Tick(automatic,ref state,new(TriggerHeld:true)),WeaponEvents.MagazineInserted|WeaponEvents.Shot,"Preserved chamber allows insert-completion fire.");Require(state.Magazine==1&&state.Chamber==1&&state.Reserve==0,"Completion shot feeds from the newly inserted magazine.");
        state=Ready(3,0,0)with{Phase=WeaponPhase.Chambering,TicksRemaining=1};
        Events(Tick(automatic,ref state,new(TriggerHeld:true)),WeaponEvents.Chambered|WeaponEvents.Shot,"Chamber completion can fire and feed once.");Require(state.Magazine==1&&state.Chamber==1&&state.AcceptedShots==1,"Chamber/fire/feed inventory is conservative.");
        var semi=Definition();state=Ready()with{Phase=WeaponPhase.Equipping,TicksRemaining=1,Cooldown=1};Events(Tick(semi,ref state,new(TriggerHeld:true)),WeaponEvents.Equipped|WeaponEvents.Shot,"Fresh semi edge may fire on equip/cooldown completion tick.");
        state=Ready(4,1,0);Events(Tick(semi,ref state,new(TriggerHeld:true,ReloadPressed:true)),WeaponEvents.Shot,"Ineligible full reload does not suppress fire.");
        state=Ready(0,0,0);var dry=Tick(automatic,ref state,new(TriggerHeld:true,ReloadPressed:true));Events(dry,WeaponEvents.DryFire,"Empty inventory cannot reload and dry fire is not a shot.");Require(state.AcceptedShots==0&&state.Cooldown==automatic.FireIntervalTicks,"Dry fire has cadence but no accepted shot.");
        Events(Tick(automatic,ref state,new(TriggerHeld:true)),WeaponEvents.None,"Dry fire respects cooldown.");Tick(automatic,ref state,new(TriggerHeld:true));Events(Tick(automatic,ref state,new(TriggerHeld:true)),WeaponEvents.DryFire,"Automatic dry fire resumes only at cadence.");
        var burst=Definition(WeaponFireMode.Burst);state=Ready(0,1,0);Events(Tick(burst,ref state,new(TriggerHeld:true)),WeaponEvents.Shot,"Burst can spend its final chambered round.");Tick(burst,ref state);Tick(burst,ref state);Events(Tick(burst,ref state),WeaponEvents.DryFire,"Empty ongoing burst stops at its next attempted round.");Require(state.BurstRemaining==0&&state.AcceptedShots==1,"Dry burst never creates a phantom shot.");
        for(int i=0;i<8;++i)Events(Tick(burst,ref state),WeaponEvents.None,"Cancelled dry burst cannot continue without a new trigger edge.");
    }
    private static void ValidationAndOverflow()
    {
        var d=Definition();var s=Ready();int large=Weapons.MaxTicks+1;
        WeaponDefinition[] invalid=[d with{MagazineCapacity=0},d with{MagazineCapacity=4097},d with{FireIntervalTicks=0},d with{FireIntervalTicks=large},d with{ReloadEjectTicks=0},d with{ReloadEjectTicks=large},d with{ReloadInsertTicks=0},d with{ReloadInsertTicks=large},d with{ChamberTicks=0},d with{ChamberTicks=large},d with{EquipTicks=0},d with{EquipTicks=large},d with{FireMode=(WeaponFireMode)99},d with{BurstCount=0},d with{BurstCount=17}];
        foreach(var bad in invalid)Reject(bad,s,new(Holster:true));
        WeaponState[] states=[s with{Magazine=-1},s with{Magazine=5},s with{Chamber=-1},s with{Chamber=2},s with{Reserve=-1},s with{Reserve=Weapons.MaxAmmo+1},s with{Magazine=4,Chamber=1,Reserve=Weapons.MaxAmmo-4},s with{AcceptedShots=-1},s with{Phase=(WeaponPhase)99},s with{TicksRemaining=1},s with{Phase=WeaponPhase.Holstered,TicksRemaining=1},s with{Cooldown=-1},s with{Cooldown=4},s with{BurstRemaining=-1},s with{BurstRemaining=4},s with{BurstRemaining=1}];
        foreach(var bad in states)Reject(d,bad,new(Holster:true));
        foreach(var phase in new[]{WeaponPhase.Equipping,WeaponPhase.ReloadEject,WeaponPhase.ReloadInsert,WeaponPhase.Chambering})
        {
            int duration=phase==WeaponPhase.ReloadInsert?3:2;var phaseState=s with{Phase=phase,Magazine=phase==WeaponPhase.ReloadInsert?0:2,Chamber=phase==WeaponPhase.Chambering?0:1};
            Reject(d,phaseState with{TicksRemaining=0});Reject(d,phaseState with{TicksRemaining=duration+1});
            Reject(Definition(WeaponFireMode.Burst),phaseState with{TicksRemaining=1,BurstRemaining=1});
        }
        Reject(Definition(WeaponFireMode.Automatic),s with{BurstRemaining=1});
        Reject(d,s with{Phase=WeaponPhase.ReloadInsert,TicksRemaining=1,Magazine=1});
        Reject(d,s with{Phase=WeaponPhase.Chambering,TicksRemaining=1,Chamber=1});
        Reject(d,s with{Phase=WeaponPhase.Chambering,TicksRemaining=1,Magazine=0,Chamber=0});
        var fast=Definition(WeaponFireMode.Automatic,4,1);s=Ready(2,1,0)with{AcceptedShots=long.MaxValue};Reject(fast,s,new(TriggerHeld:true));
        var max=Ready(0,0,0)with{AcceptedShots=long.MaxValue};Events(Tick(fast,ref max,new(TriggerHeld:true)),WeaponEvents.DryFire,"Dry fire does not overflow an unchanged accepted-shot count.");
        s=Ready(2,1,0)with{AcceptedShots=long.MaxValue-1};Events(Tick(fast,ref s,new(TriggerHeld:true)),WeaponEvents.Shot,"The last representable shot is legal.");Require(s.AcceptedShots==long.MaxValue,"Accepted shot counter reaches exact maximum.");Reject(fast,s,new(TriggerHeld:true));
        var broad=d with{MagazineCapacity=4096,BurstCount=16,FireMode=WeaponFireMode.Burst};s=Ready(4096,1,0);Events(Tick(broad,ref s,new(TriggerHeld:true)),WeaponEvents.Shot,"Inclusive magazine and burst-count maxima are accepted.");
        var limit=d with{FireIntervalTicks=Weapons.MaxTicks,ReloadEjectTicks=Weapons.MaxTicks,ReloadInsertTicks=Weapons.MaxTicks,ChamberTicks=Weapons.MaxTicks,EquipTicks=Weapons.MaxTicks};s=Ready(0,0,Weapons.MaxAmmo);Tick(limit,ref s,new(ReloadPressed:true));Require(s.TicksRemaining==Weapons.MaxTicks&&Ammo(s)==Weapons.MaxAmmo,"Inclusive domain maxima are legal without overflow.");
    }
    private static uint Next(ref uint seed){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
    private static WeaponInput Input(uint bits)=>new((bits&1)!=0,(bits&14)==2,(bits&48)==16,(bits&448)==64,(bits&1536)==512);
    private static void TraceInvariants()
    {
        foreach(var mode in new[]{WeaponFireMode.SemiAutomatic,WeaponFireMode.Automatic,WeaponFireMode.Burst})
        for(uint trial=1;trial<=24;++trial)
        {
            var d=new WeaponDefinition(1+(int)(trial%11),1+(int)(trial%5),1+(int)(trial%4),1+(int)(trial%3),1+(int)(trial%2),1+(int)(trial%3),mode,1+(int)(trial%7));
            var state=Ready(d.MagazineCapacity/2,1,30)with{AcceptedShots=17};long conserved=Ammo(state)+state.AcceptedShots;uint seed=unchecked(trial*0x9e3779b9u);
            for(int tick=0;tick<256;++tick)
            {
                var input=Input(Next(ref seed));var before=state;var step=Tick(d,ref state,input);bool shot=Has(step.Events,WeaponEvents.Shot);
                Require(Weapons.Step(in d,in before,in input)==step,"Identical primitive operands must produce identical complete state/events.");
                Require(Ammo(state)+state.AcceptedShots==conserved,"Every trace step conserves inventory plus accepted shots.");
                Require(state.AcceptedShots-before.AcceptedShots==(shot?1:0)&&Ammo(before)-Ammo(state)==(shot?1:0),"Only Shot spends one round and increments the counter.");
                Require(state.Magazine>=0&&state.Magazine<=d.MagazineCapacity&&state.Chamber is 0 or 1&&state.Reserve>=0&&state.TriggerHeld==input.TriggerHeld,"All inventory/held fields stay valid, including busy ticks.");
                Require(((int)step.Events&~1023)==0&&(!shot||state.Phase==WeaponPhase.Ready&&state.Cooldown==d.FireIntervalTicks&&before.Cooldown<=1),"Shot event and cadence domains remain exact.");
                Require(!(shot&&Has(step.Events,WeaponEvents.DryFire)),"A dry fire cannot also be an accepted shot.");
                Require(state.BurstRemaining==0||mode==WeaponFireMode.Burst&&state.Phase==WeaponPhase.Ready,"Only ready burst mode retains pending burst state.");
                if(input.Holster)Require(state.Phase==WeaponPhase.Holstered&&!shot&&Ammo(state)==Ammo(before),"Holster always wins without inventory loss.");
                if(Has(step.Events,WeaponEvents.MagazineEjected))Require(state.Magazine==0&&state.Reserve==before.Reserve+before.Magazine&&state.Chamber==before.Chamber,"Eject event proves one conservative transfer.");
                if(Has(step.Events,WeaponEvents.ReloadStarted))Require(!shot&&state.Phase is WeaponPhase.ReloadEject or WeaponPhase.Chambering,"Reload start cannot also fire/skip its selected stage.");
                if(Has(step.Events,WeaponEvents.DryFire))Require(state.Chamber==0&&state.BurstRemaining==0&&state.Cooldown==d.FireIntervalTicks,"Dry-fire state has no chamber/burst and a real cooldown.");
            }
        }
    }
    private static void CompleteSplitReplay()
    {
        foreach(var mode in new[]{WeaponFireMode.SemiAutomatic,WeaponFireMode.Automatic,WeaponFireMode.Burst})
        foreach(int split in new[]{0,1,3,7,19,64,127})
        {
            var d=Definition(mode);var first=Ready(1,1,23);uint seed=0x12345678;var inputs=new WeaponInput[192];
            for(int i=0;i<inputs.Length;++i)inputs[i]=Input(Next(ref seed));
            for(int i=0;i<split;++i)Tick(d,ref first,inputs[i]);
            byte[] bytes=JsonSerializer.SerializeToUtf8Bytes(first);var second=JsonSerializer.Deserialize<WeaponState>(bytes);
            Require(JsonSerializer.SerializeToUtf8Bytes(second).SequenceEqual(bytes),"All nine primitive state fields roundtrip, with held/burst/timers retained.");
            for(int i=split;i<inputs.Length;++i)
            {
                var a=Tick(d,ref first,inputs[i]);var b=Tick(d,ref second,inputs[i]);
                Require(a==b&&JsonSerializer.SerializeToUtf8Bytes(a).SequenceEqual(JsonSerializer.SerializeToUtf8Bytes(b)),"Split replay preserves complete state and events, not a selected ammo projection.");
            }
        }
    }
    public static void Main()
    {
        ContractAndCadence();ReloadStages();CancelCommittedStages();HolsterEquipAndEdges();CompletionAndDryFire();ValidationAndOverflow();TraceInvariants();CompleteSplitReplay();
        Console.WriteLine(JsonSerializer.Serialize(new{passed=true,groups=8,checks,scope="Independent pure weapon action tables, conservation and complete caller-state replay. No native gameplay/save/runtime/renderer or performance execution claim."}));
    }
}
