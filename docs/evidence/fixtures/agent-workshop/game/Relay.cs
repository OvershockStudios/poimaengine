using Poima;
using System.Numerics;
namespace WorkshopRelay;
[GameplayBuffer(typeof(int),3)] public partial struct ItemBuffer {}
[GameplayComponent("a1000000000000000000000000000001")] public partial struct Inventory {
 [GameplayField("a1000000000000000000000000000002")] public ItemBuffer Items;
}
[GameplayComponent("a2000000000000000000000000000001")] public partial struct Pickup {
 [GameplayField("a2000000000000000000000000000002")] public int ItemKind;
}
public struct RelayState { public EntityId Player,Camera,First,Second; public int Delivered,Won,Paused,DropPending,Collected,Dropped,Rejected,Started,Message; }
[GameModule("workshop.relay.v1")] public sealed class RelayGame : Game<RelayState> {
 static EntityId E(int n)=>EntityId.Parse("b100000000000000000000000000"+n.ToString("x4"));
 static UiId U(int n)=>UiId.Parse("c100000000000000000000000000"+n.ToString("x4"));
 static TemplateId T(int n)=>TemplateId.Parse("d100000000000000000000000000"+n.ToString("x4"));
 public override void Initialize(ref RelayState s) { s=default;s.Player=E(1);s.Camera=E(2);s.First=E(3);s.Second=E(4); }
 static string Message(int m)=>m switch {1=>"Part packed.",2=>"Capacity 3: drop the last part first.",3=>"Wrong recipe: inventory kept.",4=>"Station 1 must be completed first.",5=>"Delivery accepted.",6=>"Last part dropped nearby.",7=>"No part to drop.",8=>"No usable target within 3 m.",9=>"WORKSHOP RELAY COMPLETE",_=>"WASD move | mouse look | E use | A ivory / B wine"};
 public override void Control(ref RelayState s, ControlContext c) {
  switch(c.Action) {
   case "drop": if(s.Won==0) s.DropPending=1;break;
   case "pause":s.Paused=1;c.RequestPause();c.SetUi(U(9),visible:true);c.SetModal(U(9));break;
   case "resume":s.Paused=0;c.RequestResume();c.SetUi(U(9),visible:false);c.SetModal(null);break;
   case "save":c.SetUi(U(3),text:c.TryRequestSave("workshop-checkpoint").Accepted?"Checkpoint saved.":"Save rejected.");break;
   case "load":c.SetUi(U(3),text:c.TryRequestLoad("workshop-checkpoint").Accepted?"Loading checkpoint.":"Load rejected.");break;
  }
 }
 public override void Tick(ref RelayState s, GameContext c) {
  var inv=c.Get<Inventory>(s.Player);bool changed=false;
  if(s.Started==0) { inv.Items.Clear();changed=true;for(int k=1;k<=2;k++)for(int i=0;i<3;i++)c.Spawn(T(k),new SpawnTransform(new Vector3d(-2+i*2,1.2,k==1?-2:-4),Quaternion.Identity,new Vector3d(.5,.5,.5)));s.Started=1; }
  if(s.Won==0 && s.Paused==0) {
   bool dropping=s.DropPending!=0;s.DropPending=0;
   if(dropping) {
    if(inv.Items.Count>0) {int kind=inv.Items[inv.Items.Count-1];var p=c.Get(s.Player).Transform.Position;var f=c.Get(s.Camera).Transform.Forward;double l=Math.Sqrt(f.X*f.X+f.Z*f.Z);double x=l>.01?f.X/l:0,z=l>.01?f.Z/l:-1;c.Spawn(T(kind),new SpawnTransform(new Vector3d(p.X+x*1.1,1.2,p.Z+z*1.1),Quaternion.Identity,new Vector3d(.5,.5,.5)));inv.Items.RemoveAt(inv.Items.Count-1);changed=true;s.Dropped++;s.Message=6;}else s.Message=7;
   } else if(c.Pressed(s.Player,GameAction.Use) && s.Started!=0) {
    var cam=c.Get(s.Camera).Transform;var hit=c.Raycast(cam.Position,cam.Forward,3,new[]{s.Player});
    if(hit is null)s.Message=8;
    else if(hit.Value.Entity==s.First || hit.Value.Entity==s.Second) {
     int target=hit.Value.Entity==s.First?0:1;int a=0,b=0;for(int i=0;i<inv.Items.Count;i++){if(inv.Items[i]==1)a++;if(inv.Items[i]==2)b++;}
     if(target!=s.Delivered){s.Rejected++;s.Message=4;}
     else if(inv.Items.Count!=3 || a!=(target==0?2:1) || b!=(target==0?1:2)){s.Rejected++;s.Message=3;}
     else {inv.Items.Clear();changed=true;s.Delivered++;s.Message=5;if(s.Delivered==2){s.Won=1;s.Message=9;}}
    } else if(c.TryGet<Pickup>(hit.Value.Entity,out var part)) {
     if(inv.Items.TryAdd(part.ItemKind)){c.Despawn(hit.Value.Entity);changed=true;s.Collected++;s.Message=1;}else{s.Rejected++;s.Message=2;}
    }else s.Message=8;
   }
  } else if(s.Won!=0)s.DropPending=0;
  if(changed)c.Set(s.Player,in inv);
  string letters="";for(int i=0;i<inv.Items.Count;i++)letters+=(i>0?"  ":"")+(inv.Items[i]==1?"A":"B");
  c.SetUi(U(1),text:$"PACK [ {letters} ]   {inv.Items.Count}/3");
  c.SetUi(U(2),text:s.Won==1?"RELAY COMPLETE • 2/2":$"DELIVERED {s.Delivered}/2 | 1: A A B → 2: A B B");
  c.SetUi(U(3),text:Message(s.Message));
 }
}
