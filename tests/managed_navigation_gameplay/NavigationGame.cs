// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Verification;

[GameplayBuffer(typeof(float),30)] public partial struct RouteCoordinates { }
[GameplayComponent("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa64")]
public partial struct RouteData
{
    [GameplayField("00000000000000000000000000000001")] public RouteCoordinates Coordinates;
    [GameplayField("00000000000000000000000000000002")] public int Cursor;
}
public struct NavigationState
{
    public int Ticks,Plans,RouteValid,Arrived,Paused,Mode,QuotaRejected,QuotaReset,FailedAttempts;
    public int FailAtTick,FailureSentinel,Replans,LastStatus,LastCount;
    public double GoalX,GoalZ,LastStartX,LastStartZ;
    public EntityId Agent;
}

[GameModule("poima.verification.navigation-room")]
public sealed class NavigationGame : Game<NavigationState>,INavigationGame,ICharacterInputGame
{
    static readonly EntityId Agent=new(0,300);
    static void Stamp(string value)
    {
        var path=Environment.GetEnvironmentVariable("POIMA_NAVIGATION_SENTINEL");
        if(!string.IsNullOrEmpty(path))File.AppendAllText(path,value+"\n");
    }
    public NavigationGame()=>Stamp("constructor");
    public override void Initialize(ref NavigationState state)
    {
        Stamp("initialize");
        if(Environment.GetEnvironmentVariable("POIMA_NAVIGATION_FORBID_INITIALIZE")=="1")
            throw new InvalidOperationException("Navigation Initialize forbidden during exact restore.");
        state=default;state.Agent=Agent;state.GoalX=4;
    }
    static void Check(bool condition,string message){if(!condition)throw new InvalidOperationException(message);}
    static double Distance(double x,double z)=>Math.Sqrt(x*x+z*z);
    static double Wrap(double angle)
    {
        while(angle>180)angle-=360;while(angle < -180)angle+=360;return angle;
    }
    static void Validate(in RouteData route)
    {
        Check(route.Coordinates.Count%3==0,"Route coordinates must be XYZ triplets.");
        Check(route.Cursor>=0 && route.Cursor<=route.Coordinates.Count/3,"Invalid persisted route cursor.");
        for(int i=0;i<route.Coordinates.Count;++i)Check(float.IsFinite(route.Coordinates[i]),"Invalid persisted route coordinate.");
    }
    static NavigationPathResult Query(in NavigationState state,GameContext context,Span<NavigationPoint> points)
        =>context.FindNavigationPath(state.Agent,new(state.GoalX,0,state.GoalZ),points,new(.5,2,.5),256,4096);
    public override void Tick(ref NavigationState state,GameContext context)
    {
        if(state.Ticks==0)Stamp("tick");
        if(state.Paused!=0)return;
        ++state.Ticks;
        Span<NavigationPoint> points=stackalloc NavigationPoint[10];
        if(state.Mode==1)
        {
            for(int i=0;i<8;++i)Check(Query(in state,context,points).Complete,"Valid query failed before eight-attempt limit.");
            var sentinel=new NavigationPoint(111,222,333);points.Fill(sentinel);bool failed=false;
            try{Query(in state,context,points);}
            catch(InvalidOperationException error) when(error.Message.Contains("eight navigation query",StringComparison.Ordinal)){failed=true;}
            Check(failed,"Ninth query did not reach native attempt quota.");
            for(int i=0;i<points.Length;++i)Check(points[i]==sentinel,"Failed native quota overwrote caller span.");
            ++state.QuotaRejected;state.Mode=2;return;
        }
        if(state.Mode==2)
        {
            Check(Query(in state,context,points).Complete,"Navigation quota did not reset on next Tick.");
            ++state.QuotaReset;state.Mode=0;return;
        }
        if(state.Mode==3)
        {
            var sentinel=new NavigationPoint(111,222,333);points.Fill(sentinel);bool failed=false;
            // A valid request reaches the actual native callback but the floor
            // entity is not a CharacterController. Failed attempts consume quota.
            try{context.FindNavigationPath(new(0,1),new(4,0,0),points);}
            catch(InvalidOperationException error) when(error.Message.Contains("CharacterController",StringComparison.Ordinal)){failed=true;}
            Check(failed,"Non-character navigation query was accepted.");
            for(int i=0;i<points.Length;++i)Check(points[i]==sentinel,"Failed query overwrote caller span.");
            for(int i=0;i<7;++i)Check(Query(in state,context,points).Complete,"Valid query after failed attempt was rejected early.");
            failed=false;try{Query(in state,context,points);}
            catch(InvalidOperationException error) when(error.Message.Contains("eight navigation query",StringComparison.Ordinal)){failed=true;}
            Check(failed,"Failed native attempt did not consume quota.");++state.FailedAttempts;state.Mode=2;return;
        }
        if(state.Arrived!=0)return;
        var route=context.Get<RouteData>(state.Agent);Validate(in route);
        if(state.RouteValid==0)
        {
            var answer=Query(in state,context,points);
            Check(answer.Complete && answer.CornerCount>=1 && answer.CornerCount<=10,"Fixture route must be complete and fit the persisted ten-corner buffer.");
            route.Coordinates.Clear();route.Cursor=0;
            for(int i=0;i<answer.CornerCount;++i)
            {
                Check(route.Coordinates.TryAdd(points[i].X) && route.Coordinates.TryAdd(points[i].Y) && route.Coordinates.TryAdd(points[i].Z),"Route buffer capacity mismatch.");
            }
            state.LastStatus=(int)answer.Status;state.LastCount=answer.CornerCount;
            state.LastStartX=answer.RequestedStart.X;state.LastStartZ=answer.RequestedStart.Z;
            state.RouteValid=1;++state.Plans;
        }
        var native=context.Get(state.Agent).Transform;var position=native.Position;
        int initialCursor=route.Cursor;
        while(route.Cursor<route.Coordinates.Count/3 && Distance(route.Coordinates[route.Cursor*3]-position.X,route.Coordinates[route.Cursor*3+2]-position.Z)<.08)
            ++route.Cursor;
        if(route.Cursor>=route.Coordinates.Count/3)state.Arrived=1;
        else
        {
            double dx=route.Coordinates[route.Cursor*3]-position.X,dz=route.Coordinates[route.Cursor*3+2]-position.Z;
            double desired=Math.Atan2(-dx,-dz)*180/Math.PI;
            double current=Math.Atan2(-native.Forward.X,-native.Forward.Z)*180/Math.PI;
            double turn=Wrap(desired-current),distance=Distance(dx,dz);
            float movement=Math.Abs(turn)>4 ? 0 : (float)Math.Min(.75,distance/.08);
            context.SetCharacterInput(state.Agent,0,movement,(float)Math.Clamp(turn,-8,8));
        }
        // The observable state, component write and native movement are all
        // deliberately staged before a late failure in a multi-tick batch.
        if(state.FailAtTick>0 && context.Tick==(ulong)state.FailAtTick)
        {
            ++state.FailureSentinel;route.Cursor=Math.Min(route.Coordinates.Count/3,initialCursor+1);context.Set(state.Agent,in route);
            throw new InvalidOperationException("Intentional late navigation Tick failure.");
        }
        context.Set(state.Agent,in route); // Exactly one staged write per component target.
    }
    public override void Control(ref NavigationState state,ControlContext context)
    {
        switch(context.Action)
        {
            case "quota":state.Mode=1;break;
            case "failed_attempt":state.Mode=3;break;
            case "fail":state.FailAtTick=checked((int)context.Tick+3);break;
            case "clear_failure":state.FailAtTick=0;break;
            case "replan":state.GoalX=state.GoalX>0 ? -4 : 4;state.GoalZ=0;state.RouteValid=0;state.Arrived=0;++state.Replans;break;
            case "pause":state.Paused=1;context.RequestPause();break;
            case "resume":state.Paused=0;context.RequestResume();break;
            case "save":Check(context.TryRequestSave("navigation-room").Accepted,"Compiled save was not admitted.");break;
            case "load":Check(context.TryRequestLoad("navigation-room").Accepted,"Compiled load was not admitted.");break;
            default:throw new InvalidOperationException("Unknown navigation fixture control.");
        }
    }
}
