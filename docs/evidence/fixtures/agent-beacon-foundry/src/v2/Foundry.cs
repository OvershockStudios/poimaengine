// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Beacon;
[GameplayBuffer(typeof(EntityId),4)] public partial struct WorkOrder {}
[GameplayComponent("b30ac203016a409086e0561f072117c1")]
public partial struct Line {
 [GameplayField("b30ac203016a409086e0561f072117c2")] public WorkOrder Queue;
}
[GameplayComponent("b30ac203016a409086e0561f072117d1")]
public partial struct Core {
 [GameplayField("b30ac203016a409086e0561f072117d2",Default="0")] public int Progress;
 [GameplayField("b30ac203016a409086e0561f072117d3",Default="90")] public int Required;
 [GameplayField("b30ac203016a409086e0561f072117d4",Default="0")] public int Serial;
}
[GameplayPersistence(2)] public struct State {
 [GameplayField("d10ac203016a409086e0561f0721100d",Default="0")] public int AfterUpdate;
 [GameplayField("d10ac203016a409086e0561f07211001",Default="0")] public int Output;
 [GameplayField("d10ac203016a409086e0561f07211002",Default="9")] public int Goal;
 [GameplayField("d10ac203016a409086e0561f07211003",Default="1")] public int Paused;
 [GameplayField("d10ac203016a409086e0561f07211004",Default="0")] public int Started;
 [GameplayField("d10ac203016a409086e0561f07211005",Default="0")] public int Pending;
 [GameplayField("d10ac203016a409086e0561f07211006",Default="1")] public int NextSerial;
 [GameplayField("d10ac203016a409086e0561f07211007",Default="0")] public int Feedback;
 [GameplayField("d10ac203016a409086e0561f07211008",Default="0")] public long SaveHigh;
 [GameplayField("d10ac203016a409086e0561f07211009",Default="0")] public long SaveLow;
 [GameplayField("d10ac203016a409086e0561f0721100a",Default="0")] public long SaveSequence;
 [GameplayField("d10ac203016a409086e0561f0721100b",Default="0")] public int SaveStatus;
 [GameplayField("d10ac203016a409086e0561f0721100c",Default="0")] public int Callbacks;
}
[GameModule("beacon.foundry")]
public sealed class Foundry : Game<State> {
 static readonly EntityId Owner=EntityId.Parse("a10ac203016a409086e0561f07211001");
 static readonly TemplateId Recipe=TemplateId.Parse("a10ac203016a409086e0561f07211002");
 static UiId Ui(int n)=>UiId.Parse($"e10ac203016a409086e0561f0721{n:x4}");
 const string Slot="beacon-v2";
 public override void Initialize(ref State s){s.Goal=9;s.Paused=1;s.NextSerial=1;}
 static string Notice(int f)=>f switch {1=>"Order accepted. Admission settles next tick.",2=>"LINE FULL - four cores already admitted. Order refused.",3=>"All nine orders are already covered.",4=>"Start the foundry before submitting.",5=>"Wait for admission to settle, then save.",6=>"Storage request refused or unavailable.",7=>"Save requested - waiting for durable storage.",8=>"Save complete - checkpoint stored.",9=>"Load requested.",10=>"Storage operation failed.",11=>"Checkpoint restored. Ready to continue.",_=>"Submit cores. Only the FIFO head receives work."};
 public override void Control(ref State s,ControlContext c){
  if(c.Action=="start"){s.Started=1;s.Paused=0;c.RequestResume();}
  else if(c.Action=="pause"){s.Paused=1-s.Paused;}
  else if(c.Action=="submit"){
   var q=c.Get<Line>(Owner).Queue;
   if(s.Started==0)s.Feedback=4;
   else if(q.Count+s.Pending>=4)s.Feedback=2;
   else if(s.Output+q.Count+s.Pending>=s.Goal)s.Feedback=3;
   else {s.Pending++;s.Feedback=1;}
  }
  else if(c.Action=="save" || c.Action=="load"){
   if(s.Pending!=0)s.Feedback=5;
   else {
    var r=c.Action=="save"?c.TryRequestSave(Slot):c.TryRequestLoad(Slot);
    if(r.Accepted){s.SaveHigh=r.Ticket.EpochHigh;s.SaveLow=r.Ticket.EpochLow;s.SaveSequence=r.Ticket.Sequence;s.SaveStatus=1;s.Feedback=c.Action=="save"?7:9;}
    else s.Feedback=6;
   }
  }
  c.SetUi(Ui(8),Notice(s.Feedback));
  c.SetUi(Ui(3),s.Paused==1?"PRODUCTION PAUSED | submissions and storage available":"PRODUCTION RUNNING | FIFO head only");
 }
 public override void Tick(ref State s,GameContext c){
  s.Callbacks++;
  if(s.SaveStatus==1){
   var r=c.GetSaveResult(new SaveTicket(s.SaveHigh,s.SaveLow,s.SaveSequence));
   if(r.IsTerminal){s.SaveStatus=2;s.Feedback=r.State==SaveOperationState.Succeeded?8:r.State==SaveOperationState.Expired?11:10;}
  }
  var line=c.Get<Line>(Owner);bool changed=false;
  if(s.Started==1 && s.Paused==0 && line.Queue.Count>0){
   var id=line.Queue[0];var core=c.Get<Core>(id);core.Progress++;
   if(core.Progress>=core.Required){line.Queue.RemoveAt(0);c.Despawn(id);s.Output++;changed=true;s.AfterUpdate++;}
   else c.Set(id,in core);
  }
  int admissions=s.Pending;s.Pending=0;
  for(int i=0;i<admissions;i++){
   var transform=new SpawnTransform(new Vector3d(-2.4+((s.NextSerial-1)%4)*1.6,1.25,-3),System.Numerics.Quaternion.Identity,new Vector3d(0.7,0.7,0.7));
   var id=c.Spawn(Recipe,in transform);var core=c.GetTemplate<Core>(Recipe);core.Serial=s.NextSerial++;core.Progress=0;core.Required=90;c.Set(id,in core);
   if(!line.Queue.TryAdd(id))throw new InvalidOperationException("Admission capacity invariant");changed=true;
  }
  if(changed)c.Set(Owner,in line);
  c.SetUi(Ui(2),$"BEACON CONTRACT     {s.Output} / {s.Goal} cores finished");
  c.SetUi(Ui(3),s.Output>=s.Goal?"CONTRACT COMPLETE - harbor beacons supplied":s.Paused==1?"PRODUCTION PAUSED | submissions and storage available":"PRODUCTION RUNNING | FIFO head only");
  var observed=c.Get<Line>(Owner);
  for(int i=0;i<4;i++){
   string text=$"{i+1}.  -- empty --";
   if(i<observed.Queue.Count){var core=c.Get<Core>(observed.Queue[i]);text=$"{i+1}.  CORE #{core.Serial:00}    {core.Progress} / {core.Required} ticks"+(i==0?"   [HEAD]":"   waiting");}
   c.SetUi(Ui(4+i),text);
  }
  c.SetUi(Ui(8),Notice(s.Feedback));
  c.SetUi(Ui(9),$"After update: {s.AfterUpdate} finished | 90 ticks / core");
 }
}
