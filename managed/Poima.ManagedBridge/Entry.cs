// SPDX-License-Identifier: Apache-2.0
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using Poima.Build;
using Poima;
namespace Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCall
{
    public uint Version,Operation;public ulong Handle;public byte* Text;public byte* State;public uint StateBytes,InputCount;
    public NativeServices* Services;public GameInput* Inputs;public ulong Tick;public byte* Output;public uint OutputCapacity,Reserved;
}
internal sealed class GameLoadContext(string path) : AssemblyLoadContext(isCollectible:true)
{
    private readonly AssemblyDependencyResolver resolver=new(path);
    protected override Assembly? Load(AssemblyName name)
    {
        if(name.Name==typeof(Game<>).Assembly.GetName().Name)return typeof(Game<>).Assembly;
        string? file=resolver.ResolveAssemblyToPath(name);
        return file is null ? null : LoadFromStream(new MemoryStream(File.ReadAllBytes(file)));
    }
}
internal sealed record FieldDescription(string name,string kind,int offset,int bytes);
internal sealed record Manifest(ulong handle,string identity,string assembly_sha256,int bytes,FieldDescription[] fields,
    [property: JsonIgnore(Condition=JsonIgnoreCondition.WhenWritingNull)] JsonElement? components=null,
    [property: JsonIgnore(Condition=JsonIgnoreCondition.WhenWritingNull)] JsonElement? persistent=null,
    [property: JsonIgnore(Condition=JsonIgnoreCondition.WhenWritingNull)] JsonElement? requirements=null);
internal sealed record Module(GameLoadContext Context,IGame Game,int Bytes,GameplayRequiredFeatures RequiredFeatures);
public static unsafe class Entry
{
    private static readonly Dictionary<ulong,Module> modules=[];
    private static readonly List<WeakReference> retired=[];
    private static ulong next=1;
    static Entry() { Console.SetOut(Console.Error); }
    public static int Invoke(nint arguments,int bytes)
    {
        NativeCall* call=(NativeCall*)arguments;
        if(call==null || bytes!=sizeof(NativeCall) || call->Version!=1 || call->Output==null || call->OutputCapacity<2048)return -1;
        call->Output[0]=0;
        try
        {
            if(sizeof(NativeCall)!=80 || sizeof(NativeServices)!=176 || sizeof(NativeSound)!=32 || sizeof(GameInput)!=40 || sizeof(EntitySnapshot)!=160 || sizeof(NativeRay)!=72 || sizeof(NativeHit)!=88 || sizeof(NativeMotion)!=80 ||
                sizeof(NativeAnimationCommand)!=48 || sizeof(NativeAnimationTransition)!=56 || sizeof(NativeAnimationState)!=120 || !AnimationLayout.Valid || !AnimationServiceAbi.LayoutValid() || !AnimationLayerServiceAbi.LayoutValid() || !CharacterInputServiceAbi.LayoutValid() || !NavigationServiceAbi.LayoutValid() || !SaveAbiLayout.Valid() || !ComponentAbiLayout.Valid() || !LifecycleAbiLayout.Valid() || !UiAbiLayout.Valid())
                throw new InvalidOperationException("Gameplay ABI layout mismatch.");
            switch(call->Operation)
            {
                case 1: Load(call);break;
                case 2: Run(call,false);break;
                case 3: Run(call,true);break;
                case 6: Run(call,true,true);break;
                case 4: Release(call->Handle);break;
                case 5:
                    for(int i=0;i<8 && retired.Any(r=>r.IsAlive);++i) { GC.Collect();GC.WaitForPendingFinalizers();GC.Collect(); }
                    int alive=retired.Count(r=>r.IsAlive);retired.RemoveAll(r=>!r.IsAlive);
                    Write(call,JsonSerializer.Serialize(new {active_modules=modules.Count,retired_alive=alive}));break;
                default: throw new ArgumentException("Unknown managed bridge operation.");
            }
            return 0;
        }
        catch(Exception error)
        {
            string message=error.GetBaseException().Message;
            if(message.Length>450)message=message[..450];
            Write(call,message);return -1;
        }
    }
    private static class AnimationLayout
    {
        // Verify tail offsets as well as total sizes; checking sizes alone can
        // miss mismatched field ordering across native and managed builds.
        internal static readonly bool Valid=
            Marshal.OffsetOf<NativeServices>(nameof(NativeServices.Context)).ToInt64()==8 &&
            Marshal.OffsetOf<NativeServices>(nameof(NativeServices.Sound)).ToInt64()==40 &&
            Marshal.OffsetOf<NativeServices>(nameof(NativeServices.AnimationGet)).ToInt64()==48 &&
            Marshal.OffsetOf<NativeServices>(nameof(NativeServices.AnimationSet)).ToInt64()==56 &&
            Marshal.OffsetOf<NativeAnimationCommand>(nameof(NativeAnimationCommand.Clip)).ToInt64()==32 &&
            Marshal.OffsetOf<NativeAnimationCommand>(nameof(NativeAnimationCommand.BlendTicks)).ToInt64()==44 &&
            Marshal.OffsetOf<NativeAnimationTransition>(nameof(NativeAnimationTransition.DurationTicks)).ToInt64()==32 &&
            Marshal.OffsetOf<NativeAnimationTransition>(nameof(NativeAnimationTransition.SourceClip)).ToInt64()==40 &&
            Marshal.OffsetOf<NativeAnimationState>(nameof(NativeAnimationState.Present)).ToInt64()==44 &&
            Marshal.OffsetOf<NativeAnimationState>(nameof(NativeAnimationState.Transition)).ToInt64()==64;
    }
    private static void Write(NativeCall* call,string text)
    {
        int bytes=Encoding.UTF8.GetByteCount(text);
        if(bytes>=call->OutputCapacity)throw new InvalidOperationException("Bridge output exceeds its bounded buffer.");
        Encoding.UTF8.GetBytes(text,new Span<byte>(call->Output,bytes));call->Output[bytes]=0;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Load(NativeCall* call)
    {
        using var request=JsonDocument.Parse(Marshal.PtrToStringUTF8((nint)call->Text) ?? "");
        string path=Path.GetFullPath(request.RootElement.GetProperty("assembly").GetString()!);
        string typeName=request.RootElement.GetProperty("type").GetString()!;
        if(new FileInfo(path).Length>64*1024*1024)throw new ArgumentException("Game assembly exceeds 64 MiB.");
        if(modules.Count>=32 || retired.Count>=1024)throw new InvalidOperationException("Managed context budget reached; collect diagnostics or restart the session.");
        var context=new GameLoadContext(path);bool published=false;
        try
        {
            byte[] image=File.ReadAllBytes(path);var components=ComponentMetadata.ReadSchemas(image);using var stream=new MemoryStream(image);var assembly=context.LoadFromStream(stream);
            Type type=assembly.GetType(typeName,true)!;
            if(type.IsAbstract || !typeof(IGame).IsAssignableFrom(type))throw new ArgumentException("Type must derive from Game<TState>.");
            bool inertial=typeof(IInertialAnimationGame).IsAssignableFrom(type);
            bool masked=typeof(IMaskedAnimationGame).IsAssignableFrom(type);
            bool character=typeof(ICharacterInputGame).IsAssignableFrom(type);
            bool navigation=typeof(INavigationGame).IsAssignableFrom(type);
            var required=GameplayRequirements.Features(inertial,masked,character,navigation);
            GameplayRequirements.ValidateHost(request.RootElement,required);
            for(Type? current=type;current!=null;current=current.BaseType)
                if(current.GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.DeclaredOnly).Length!=0)
                    throw new ArgumentException("Game classes must be stateless; declare mutable data in TState.");
            var identity=PersistenceMetadata.GameIdentity(type);
            IGame game=(IGame)Activator.CreateInstance(type)!;Type state=game.StateType;
            if(!state.IsLayoutSequential || game.StateBytes<1 || game.StateBytes>65536 || Marshal.SizeOf(state)!=game.StateBytes)
                throw new ArgumentException("State must have sequential blittable layout and contain 1..65536 bytes.");
            var fields=state.GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic);
            if(fields.Length is <1 or >128 || fields.Any(f=>!f.IsPublic || f.IsInitOnly || f.Name.Length>64))throw new ArgumentException("State requires 1..128 public mutable fields with names at most 64 characters.");
            var descriptions=new List<FieldDescription>();
            foreach(var field in fields)
            {
                var (kind,size)=field.FieldType==typeof(int) ? ("int32",4) : field.FieldType==typeof(long) ? ("int64",8) : field.FieldType==typeof(float) ? ("float32",4) : field.FieldType==typeof(double) ? ("float64",8) : field.FieldType==typeof(EntityId) ? ("entity",16) : throw new ArgumentException($"Unsupported state field {field.Name}: use int, long, float, double or EntityId.");
                descriptions.Add(new(field.Name,kind,Marshal.OffsetOf(state,field.Name).ToInt32(),size));
            }
            var persistent=PersistenceMetadata.Read(state);
            JsonElement? requirements=null;
            if(required!=GameplayRequiredFeatures.None) { using var declaration=JsonDocument.Parse(GameplayRequirements.Json(required));requirements=declaration.RootElement.Clone(); }
            ulong handle=next++;var manifest=new Manifest(handle,identity,Convert.ToHexStringLower(SHA256.HashData(image)),game.StateBytes,descriptions.OrderBy(f=>f.name,StringComparer.Ordinal).ToArray(),components,persistent,requirements);
            string encoded=JsonSerializer.Serialize(manifest); // Stable bridge DTOs only; don't cache game Types in serialization.
            Write(call,encoded);modules.Add(handle,new(context,game,game.StateBytes,required));published=true;
        }
        finally { if(!published) { retired.Add(new(context));context.Unload(); } }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Run(NativeCall* call,bool tick,bool control=false)
    {
        Module module=modules[call->Handle];
        if(call->State==null || call->StateBytes!=module.Bytes)throw new ArgumentException("Gameplay state size mismatch.");
        Span<byte> state=new(call->State,module.Bytes);
        if(!tick) { module.Game.Initialize(state);return; }
        ServiceAbi.Validate(call->Services,call->Inputs,call->InputCount);
        GameplayRequirements.ValidateServices(call->Services,module.RequiredFeatures);
        if(control) {
            if(call->InputCount!=0 || call->Inputs!=null)throw new ArgumentException("Control callbacks cannot carry physics input.");
            module.Game.Control(state,new ControlContext(call->Services,call->Tick));
        } else module.Game.Tick(state,new GameContext(call->Services,call->Inputs,(int)call->InputCount,call->Tick,module.RequiredFeatures));
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Release(ulong handle)
    {
        if(modules.Remove(handle,out Module? module)) { retired.Add(new(module.Context));module.Context.Unload(); }
    }
}
