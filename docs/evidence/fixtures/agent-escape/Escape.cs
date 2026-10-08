using Poima;
using System.Runtime.InteropServices;
namespace PrismEscape;
[StructLayout(LayoutKind.Sequential)]
public struct State {
 public EntityId Player,Camera,Exit,RedKey,GreenKey,BlueKey;
 public int Initialized,CollectedCount,Escaped,LockedAttempts,Paused;
 public long TicketHigh,TicketLow,TicketSequence;
 public int LastSaveOutcome,Restores;
}
[GameModule("prism.three-key-escape")]
public sealed class EscapeGame:Game<State> {
 public override void Initialize(ref State s) {s.Player=new(0,1);s.Camera=new(0,2);s.Exit=new(0,3);}
 static UiId Ui(ulong n)=>new(0,n);
 static string Status(in State s)=>$"PRISM VAULT | Keys {s.CollectedCount}/3 | "+(s.Escaped==1?"ESCAPED!":s.CollectedCount==3?"Exit ready: look at the gold door and press E":"Exit LOCKED: find red, green and blue keys")+$" | Locked attempts {s.LockedAttempts}";
 static void Take(ref EntityId key,ref State s,GameContext c){c.Despawn(key);key=default;s.CollectedCount++;}
 public override void Tick(ref State s,GameContext c){
  if(s.TicketSequence!=0){var r=c.GetSaveResult(new(s.TicketHigh,s.TicketLow,s.TicketSequence));if(r.IsTerminal){s.LastSaveOutcome=(int)r.State;s.TicketSequence=0;}}
  if(c.Saves.LastRestore is not null && s.Restores==0){s.Restores=1;s.Paused=0;}
  if(s.Initialized==0){s.RedKey=c.Spawn(new(0,101));s.GreenKey=c.Spawn(new(0,102));s.BlueKey=c.Spawn(new(0,103));s.Initialized=1;}
  else if(s.Paused==0 && s.Escaped==0 && c.Pressed(s.Player,GameAction.Use)){
   var cam=c.Get(s.Camera).Transform;
   var hit=c.Raycast(cam.Position,cam.Forward,3,new[]{s.Player});
   if(hit is {} h){
    if(s.RedKey!=default && h.Entity==s.RedKey)Take(ref s.RedKey,ref s,c);
    else if(s.GreenKey!=default && h.Entity==s.GreenKey)Take(ref s.GreenKey,ref s,c);
    else if(s.BlueKey!=default && h.Entity==s.BlueKey)Take(ref s.BlueKey,ref s,c);
    else if(h.Entity==s.Exit){if(s.CollectedCount==3)s.Escaped=1;else s.LockedAttempts++;}
   }
  }
  var text=Status(s);if(c.GetUi(Ui(201)).Text!=text)c.SetUi(Ui(201),text);
 }
 public override void Control(ref State s,ControlContext c){
  if(s.TicketSequence!=0){var r=c.GetSaveResult(new(s.TicketHigh,s.TicketLow,s.TicketSequence));if(r.IsTerminal){s.LastSaveOutcome=(int)r.State;s.TicketSequence=0;}}
  switch(c.Action){
   case "pause":s.Paused=1;c.SetUi(Ui(202),"Paused: Resume to continue");c.RequestPause();break;
   case "resume":s.Paused=0;c.SetUi(Ui(202),"WASD move | Mouse look | E use within 3m");c.RequestResume();break;
   case "save":case "load":
    var t=c.Action=="save"?c.RequestSave("escape-checkpoint"):c.RequestLoad("escape-checkpoint");
    s.TicketHigh=t.EpochHigh;s.TicketLow=t.EpochLow;s.TicketSequence=t.Sequence;break;
   default:throw new InvalidOperationException("Unknown control");
  }
 }
}
