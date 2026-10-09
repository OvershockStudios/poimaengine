// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
namespace Poima;
[StructLayout(LayoutKind.Sequential)]
public readonly record struct UiId(ulong High,ulong Low)
{
    public static UiId Parse(string text)
    {
        if(text is null || text.Length!=32 || text==new string('0',32) || text.Any(c=>!((c>='0' && c<='9') || (c>='a' && c<='f'))))throw new ArgumentException("UI IDs require nonzero canonical 32-hex text.");
        return new(ulong.Parse(text.AsSpan(0,16),NumberStyles.HexNumber,CultureInfo.InvariantCulture),ulong.Parse(text.AsSpan(16),NumberStyles.HexNumber,CultureInfo.InvariantCulture));
    }
    public override string ToString()=>$"{High:x16}{Low:x16}";
}
public enum UiKind:uint { Panel=0,Label=1,Button=2 }
public readonly record struct UiSnapshot(ulong Revision,UiKind Kind,string Text,bool Visible,bool Enabled,bool EffectiveVisible,bool EffectiveEnabled,bool Eligible);
[StructLayout(LayoutKind.Sequential)] internal struct NativeUiState { public ulong Revision;public uint Kind,Visible,Enabled,EffectiveVisible,EffectiveEnabled,Eligible,TextBytes,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeUiPatch { public UiId Id;public byte* Text;public uint TextBytes,Mask,Visible,Enabled; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeUiModalEdit { public UiId Id;public uint Change,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeUiControlEvent { public UiId Element;public ulong Sequence;public uint ActionBytes,Reserved;public fixed byte Action[128]; }
internal static unsafe class UiAbiLayout
{
    internal static bool Valid()
    {
        NativeServices s=default;NativeUiState v=default;NativeUiPatch p=default;NativeUiModalEdit m=default;NativeUiControlEvent e=default;
        return sizeof(UiId)==16 && sizeof(NativeServices)==176 && sizeof(NativeUiState)==40 && sizeof(NativeUiPatch)==40 && sizeof(NativeUiModalEdit)==24 && sizeof(NativeUiControlEvent)==160 &&
            (byte*)&s.UiGet-(byte*)&s==144 && (byte*)&s.UiEdit-(byte*)&s==152 && (byte*)&s.ControlInfo-(byte*)&s==160 && (byte*)&s.ControlRequest-(byte*)&s==168 &&
            (byte*)&v.Kind-(byte*)&v==8 && (byte*)&v.TextBytes-(byte*)&v==32 && (byte*)&p.Text-(byte*)&p==16 && (byte*)&p.Mask-(byte*)&p==28 &&
            (byte*)&m.Change-(byte*)&m==16 && (byte*)&e.Sequence-(byte*)&e==16 && e.Action-(byte*)&e==32;
    }
}
internal static class UiEncoding { internal static readonly UTF8Encoding Utf8=new(false,true); }
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads committed state, including within a callback that stages UI edits.</summary>
    public UiSnapshot GetUi(UiId id)
    {
        byte* text=stackalloc byte[16384];NativeUiState state=default;uint written=0;NativeError error=default;
        Check(services->UiGet(services->Context,&id,&state,text,16384,&written,&error),&error);
        if(written>16384 || state.TextBytes!=written || state.Reserved!=0 || state.Kind>2 || state.Visible>1 || state.Enabled>1 || state.EffectiveVisible>1 || state.EffectiveEnabled>1 || state.Eligible>1 || state.Revision>9007199254740991UL)
            throw new InvalidOperationException("Invalid native UI state.");
        return new(state.Revision,(UiKind)state.Kind,UiEncoding.Utf8.GetString(new ReadOnlySpan<byte>(text,(int)written)),state.Visible!=0,state.Enabled!=0,state.EffectiveVisible!=0,state.EffectiveEnabled!=0,state.Eligible!=0);
    }
    /// <summary>Stages copied literal UTF-8 text and logical flags; native publication is atomic after callback.</summary>
    public void SetUi(UiId id,string? text=null,bool? visible=null,bool? enabled=null)
    {
        if(text is null && visible is null && enabled is null)throw new ArgumentException("Empty UI patch.");
        if(text is not null && (text.Length>16384 || text.Contains('\0')))throw new ArgumentException("UI text exceeds bounds or contains NUL.");
        int length=text is null?0:UiEncoding.Utf8.GetByteCount(text);if(length>16384)throw new ArgumentException("UI text exceeds 16 KiB UTF-8.");
        byte* bytes=stackalloc byte[16384];if(text is not null)UiEncoding.Utf8.GetBytes(text,new Span<byte>(bytes,length));
        NativeUiPatch patch=new(){Id=id,Text=text is null?null:bytes,TextBytes=(uint)length,Mask=(text is null?0u:1u)|(visible.HasValue?2u:0u)|(enabled.HasValue?4u:0u),Visible=visible==true?1u:0u,Enabled=enabled==true?1u:0u};
        NativeError error=default;Check(services->UiEdit(services->Context,&patch,1,null,&error),&error);
    }
    public void SetModal(UiId? panel)
    {
        NativeUiModalEdit modal=new(){Id=panel??default,Change=1};NativeError error=default;
        Check(services->UiEdit(services->Context,null,0,&modal,&error),&error);
    }
}
/// <summary>A semantic action at unchanged simulation time. Mutable gameplay state still belongs in TState.</summary>
public readonly unsafe ref partial struct ControlContext
{
    private readonly NativeServices* services;
    private readonly GameContext context;
    public ulong Tick=>context.Tick;
    public ulong Sequence { get; }
    public UiId Element { get; }
    public string Action { get; }
    internal ControlContext(NativeServices* services,ulong tick):this(services,tick,GameplayRequiredFeatures.None) { }
    internal ControlContext(NativeServices* services,ulong tick,GameplayRequiredFeatures requiredFeatures)
    {
        this.services=services;context=new(services,null,0,tick,requiredFeatures);NativeUiControlEvent e=default;NativeError error=default;
        Check(services->ControlInfo(services->Context,&e,&error),&error);
        if(e.Reserved!=0 || e.ActionBytes is <1 or >128 || e.Sequence is <1 or >9007199254740991UL || e.Element==default)throw new InvalidOperationException("Invalid native control event.");
        for(uint i=0;i<e.ActionBytes;++i) {byte c=e.Action[i];if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='.' || c=='-'))throw new InvalidOperationException("Invalid native action token.");}
        Element=e.Element;Sequence=e.Sequence;Action=Encoding.ASCII.GetString(new ReadOnlySpan<byte>(e.Action,(int)e.ActionBytes));
    }
    private static void Check(int code,NativeError* error) {if(code!=0)throw new InvalidOperationException(Marshal.PtrToStringUTF8((nint)error->Text)??"Native control service failed.");}
    public UiSnapshot GetUi(UiId id)=>context.GetUi(id);
    public void SetUi(UiId id,string? text=null,bool? visible=null,bool? enabled=null)=>context.SetUi(id,text,visible,enabled);
    public void SetModal(UiId? panel)=>context.SetModal(panel);
    public SaveCapabilities Saves=>context.Saves;
    public SaveRequestResult TryRequestSave(string slot,long? expectedGeneration=null)=>context.TryRequestSave(slot,expectedGeneration);
    public SaveRequestResult TryRequestLoad(string slot,long? expectedGeneration=null,bool allowRecovery=false)=>context.TryRequestLoad(slot,expectedGeneration,allowRecovery);
    public SaveTicket RequestSave(string slot,long? expectedGeneration=null)=>context.RequestSave(slot,expectedGeneration);
    public SaveTicket RequestLoad(string slot,long? expectedGeneration=null,bool allowRecovery=false)=>context.RequestLoad(slot,expectedGeneration,allowRecovery);
    public SaveOperationResult GetSaveResult(SaveTicket ticket)=>context.GetSaveResult(ticket);
    public void RequestResume()=>Request(1);
    public void RequestPause()=>Request(2);
    private void Request(uint intent) {NativeError error=default;Check(services->ControlRequest(services->Context,intent,&error),&error);}
}
