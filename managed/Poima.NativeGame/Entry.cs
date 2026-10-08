// SPDX-License-Identifier: Apache-2.0
// Compiled together with the generated, statically typed Binding. No assembly loading or reflection.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
namespace Poima.NativeGame;
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCall
{
    public uint Version,Operation;public ulong Handle;public byte* Text;public byte* State;public uint StateBytes,InputCount;
    public NativeServices* Services;public GameInput* Inputs;public ulong Tick;public byte* Output;public uint OutputCapacity,Reserved;
}
public static unsafe class Entry
{
    // Per-instance game objects are stateless; authoritative state is the engine-owned byte buffer.
    private static readonly Dictionary<ulong,object> modules=[];
    private static ulong next=1;
    private static bool redirected;
    private static GameplayRequiredFeatures? declaredFeatures;
    private static GameplayRequiredFeatures RequiredFeatures()
    {
        declaredFeatures ??= GameplayRequirements.DeclaredFeatures(Binding.Requirements);
        return declaredFeatures.Value;
    }
    [UnmanagedCallersOnly(EntryPoint="poima_gameplay_entry",CallConvs=[typeof(CallConvCdecl)])]
    public static int Invoke(void* arguments,int bytes)
    {
        var call=(NativeCall*)arguments;
        if(call==null || bytes!=sizeof(NativeCall) || call->Version!=1 || call->Output==null || call->OutputCapacity<2048)return -1;
        call->Output[0]=0;
        try
        {
            if(!redirected) { Console.SetOut(Console.Error);redirected=true; }
            if(sizeof(NativeCall)!=80 || sizeof(NativeServices)!=176 || sizeof(GameInput)!=40 || sizeof(EntitySnapshot)!=160 ||
               sizeof(NativeRay)!=72 || sizeof(NativeHit)!=88 || sizeof(NativeMotion)!=80 || sizeof(NativeSound)!=32 ||
               sizeof(NativeAnimationCommand)!=48 || sizeof(NativeAnimationTransition)!=56 || sizeof(NativeAnimationState)!=120 || !Binding.LayoutValid() || !AbiLayout() || !AnimationServiceAbi.LayoutValid() || !AnimationLayerServiceAbi.LayoutValid() || !CharacterInputServiceAbi.LayoutValid() || !NavigationServiceAbi.LayoutValid() || !SaveAbiLayout.Valid() || !ComponentAbiLayout.Valid() || !LifecycleAbiLayout.Valid() || !UiAbiLayout.Valid())
                throw new InvalidOperationException("Generated gameplay ABI layout mismatch.");
            switch(call->Operation)
            {
                case 1:
                    using(var request=JsonDocument.Parse(Marshal.PtrToStringUTF8((nint)call->Text)??""))
                    {
                        if(request.RootElement.GetProperty("type").GetString()!=Binding.TypeName)throw new ArgumentException("Compiled game type differs from requested type.");
                        GameplayRequirements.ValidateHost(request.RootElement,RequiredFeatures());
                    }
                    if(modules.Count>=32 || next==ulong.MaxValue)throw new InvalidOperationException("Native module instance budget reached.");
                    ulong handle=next++;object game=Binding.Create();
                    Write(call,Binding.Schema[..^1]+",\"handle\":"+handle.ToString(System.Globalization.CultureInfo.InvariantCulture)+",\"diagnostics\":"+Diagnostics()+
                        (RequiredFeatures()!=GameplayRequiredFeatures.None ? ",\"requirements\":"+Binding.Requirements : "")+"}");
                    modules.Add(handle,game);break;
                case 2: case 3: case 6:
                    if(!modules.TryGetValue(call->Handle,out var instance))throw new ArgumentException("Unknown native game instance.");
                    if(call->State==null || call->StateBytes!=Binding.StateBytes)throw new ArgumentException("Gameplay state size mismatch.");
                    if(call->Operation==2) { Binding.InitializeObject(instance,call->State);break; }
                    ServiceAbi.Validate(call->Services,call->Inputs,call->InputCount);
                    GameplayRequirements.ValidateServices(call->Services,RequiredFeatures());
                    if(call->Operation==6) {
                        if(call->InputCount!=0 || call->Inputs!=null)throw new ArgumentException("Control callbacks cannot carry physics input.");
                        Binding.ControlObject(instance,call->State,new ControlContext(call->Services,call->Tick));
                    } else Binding.TickObject(instance,call->State,new GameContext(call->Services,call->Inputs,(int)call->InputCount,call->Tick,RequiredFeatures()));break;
                case 4: modules.Remove(call->Handle);break;
                case 5: Write(call,"{\"active_modules\":"+modules.Count.ToString(System.Globalization.CultureInfo.InvariantCulture)+",\"retired_alive\":0,\"unload_supported\":false,\"diagnostics\":"+Diagnostics()+"}");break;
                default: throw new ArgumentException("Unknown native game operation.");
            }
            return 0;
        }
        catch(Exception error)
        {
            string message=error.GetBaseException().Message;if(message.Length>450)message=message[..450];
            try { Write(call,message); }catch { call->Output[0]=0; }return -1;
        }
    }
    private static string Diagnostics()=>"{\"dynamic_code_supported\":"+(RuntimeFeature.IsDynamicCodeSupported?"true":"false")+",\"dynamic_code_compiled\":"+(RuntimeFeature.IsDynamicCodeCompiled?"true":"false")+"}";
    private static bool AbiLayout()
    {
        NativeServices services=default;NativeAnimationCommand command=default;NativeAnimationTransition transition=default;NativeAnimationState state=default;
        return (byte*)&services.Context-(byte*)&services==8 && (byte*)&services.AnimationGet-(byte*)&services==48 && (byte*)&services.AnimationSet-(byte*)&services==56 &&
            (byte*)&command.Clip-(byte*)&command==32 && (byte*)&command.BlendTicks-(byte*)&command==44 &&
            (byte*)&transition.SourceClip-(byte*)&transition==40 && (byte*)&state.Transition-(byte*)&state==64;
    }
    private static void Write(NativeCall* call,string text)
    {
        int count=Encoding.UTF8.GetByteCount(text);if(count>=call->OutputCapacity)throw new InvalidOperationException("Native game output exceeds buffer.");
        Encoding.UTF8.GetBytes(text,new Span<byte>(call->Output,count));call->Output[count]=0;
    }
}
