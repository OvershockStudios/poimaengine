// SPDX-License-Identifier: Apache-2.0
// Statically typed navigation-only binding under CoreCLR, not an AOT image.
using Poima;
namespace Poima.NativeGame;
internal static unsafe class Binding
{
    internal const string TypeName="StaticNavigationProbe",Schema="{\"state_bytes\":4}";
    internal const string Requirements="{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":224,\"features\":[\"baseline_v7\",\"navigation_query_v1\"]}";
    internal const int StateBytes=4;
    internal static bool LayoutValid()=>sizeof(int)==4;
    internal static object Create(){++NavigationEntryProbe.Constructors;return new object();}
    internal static void InitializeObject(object game,byte* state){++NavigationEntryProbe.Initializes;*(int*)state=10;}
    internal static void TickObject(object game,byte* state,GameContext context)
    {
        Span<NavigationPoint> corners=stackalloc NavigationPoint[3];corners.Fill(new(71,72,73));
        var result=context.FindNavigationPath(new(1,2),new(5,0,6),corners);
        if(result.Status!=NavigationPathStatus.Complete || result.CornerCount!=1 || corners[0]!=new NavigationPoint(5,0,6) || corners[1]!=new NavigationPoint(71,72,73))throw new Exception("Statically bound navigation reply differs.");
        ++*(int*)state;
    }
    internal static void ControlObject(object game,byte* state,ControlContext context){++*(int*)state;}
}
