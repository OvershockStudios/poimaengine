// SPDX-License-Identifier: Apache-2.0
// Test-only typed binding. The production native entry is linked unchanged;
// this fixture exercises its guards under CoreCLR, not NativeAOT publishing.
using Poima;
namespace Poima.NativeGame;
internal static unsafe class Binding
{
    internal const string TypeName="PrefixProbe",Schema="{\"state_bytes\":4}";
    internal const int StateBytes=4;
    internal static bool LayoutValid()=>sizeof(int)==4;
    internal static object Create()=>new object();
    internal static void InitializeObject(object game,byte* state)=>*(int*)state=10;
    internal static void TickObject(object game,byte* state,GameContext context)
    {
        context.SetUi(new(1,2),"New é",true,false);++*(int*)state;
    }
    internal static void ControlObject(object game,byte* state,ControlContext context)
    {
        if(context.Tick!=123 || context.Sequence!=42 || context.Element!=new UiId(1,3) || context.Action!="save")throw new Exception("Native entry control payload mismatch.");
        context.RequestPause();++*(int*)state;
    }
}
