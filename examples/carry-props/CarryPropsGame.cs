// SPDX-License-Identifier: Apache-2.0
using Poima;

namespace Poima.Examples;

[GameplayComponent("c913ebece457b13dc5ce02fe05512b62")]
public partial struct CarryPropSettings
{
    // A template and a UI label are not entities. Store their raw identity words
    // so runtime entity-reference validation only applies to actual entities.
    [GameplayField("00000000000000000000000000000001")] public long TemplateHigh;
    [GameplayField("00000000000000000000000000000002")] public long TemplateLow;
    [GameplayField("00000000000000000000000000000003")] public long PromptHigh;
    [GameplayField("00000000000000000000000000000004")] public long PromptLow;
    [GameplayField("00000000000000000000000000000005",Default="7")] public int WindupTicks;
    [GameplayField("00000000000000000000000000000006",Default="9")] public int RecoverTicks;
    [GameplayField("00000000000000000000000000000007")] public EntityId Camera;
    [GameplayField("00000000000000000000000000000008")] public EntityId StartingAnchor;

    internal readonly EntityId Template => new(unchecked((ulong)TemplateHigh), unchecked((ulong)TemplateLow));
    internal readonly UiId Prompt => new(unchecked((ulong)PromptHigh), unchecked((ulong)PromptLow));
}

[GameModule("poima.example.carry-props")]
public sealed class CarryPropsGame : Game<ReachPickupState>,
    IItemAttachmentAimGame, IItemAttachmentPickupGame, IItemAttachmentReleaseGame,
    ITwoBoneIkOrientationGame, IMaskedAnimationGame, IInputActionsGame
{
    private const int PlaceStartingProp=-1, ReleaseStartingProp=-2, ObserveStartingProp=-3;
    private const int PreparingDrop=4, AwaitingDrop=5, Released=6, Blocked=7;

    public override void Initialize(ref ReachPickupState state) { }

    public override void Tick(ref ReachPickupState state, GameContext context) => Update(ref state,context,ReachGuard.None);

    internal static void Update(ref ReachPickupState state, GameContext context, ReachGuard guard)
    {
        var settings=context.Get<CarryPropSettings>(state.Actor);
        if(settings.Camera==default || settings.Template==default ||
            settings.WindupTicks is <1 or >120 || settings.RecoverTicks is <2 or >120)
            throw new InvalidOperationException("Author a camera, release body template, 1–120 windup ticks and 2–120 recovery ticks.");

        switch(state.Phase)
        {
            case PlaceStartingProp:
                if(settings.StartingAnchor==default)
                    throw new InvalidOperationException("Author a fixed starting anchor for the starting prop.");
                context.TransferItem(state.Item,settings.StartingAnchor);
                state.Phase=ReleaseStartingProp;
                break;
            case ReleaseStartingProp:
                context.AttemptItemRelease(state.Item,settings.Template,default,default);
                state.Phase=ObserveStartingProp;
                break;
            case ObserveStartingProp:
                if(context.GetItemReleaseOutcome(state.Item)?.Status!=ItemReleaseStatus.Released)
                    throw new InvalidOperationException("The starting prop needs room for its release body.");
                state.Item=default;
                state.Phase=ReachPickupGame.Idle;
                break;
            case ReachPickupGame.Held:
                if(context.GetInputAction(state.Actor,"drop").Pressed)
                {
                    state.Phase=PreparingDrop;
                    state.TicksInPhase=0;
                    PrepareDrop(ref state,context,settings);
                }
                break;
            case PreparingDrop:
                PrepareDrop(ref state,context,settings);
                break;
            case AwaitingDrop:
                var outcome=context.GetItemReleaseOutcome(state.Item)
                    ?? throw new InvalidOperationException("The staged release outcome is missing.");
                state.Phase=outcome.Status==ItemReleaseStatus.Released ? Released : Blocked;
                state.TicksInPhase=0;
                if(state.Phase==Released)
                {
                    state.Item=default;
                    state.PickupTick=0;
                }
                Recover(ref state,context,settings.RecoverTicks);
                break;
            case Released:
            case Blocked:
                Recover(ref state,context,settings.RecoverTicks);
                break;
            default:
                ReachPickupGame.Update(ref state,context,true,settings.Camera,guard);
                break;
        }

        if(settings.Prompt!=default)UpdatePrompt(state,context,settings);
    }

    private static double Ease(double t) => t*t*(3-2*t);

    private static void PrepareDrop(ref ReachPickupState state, GameContext context, in CarryPropSettings settings)
    {
        double progress=(double)++state.TicksInPhase/settings.WindupTicks;
        state.Opening=.8*Ease(progress);
        PoseFingers(state,context);
        if(state.TicksInPhase!=settings.WindupTicks)return;
        context.AttemptItemRelease(state.Item,settings.Template,default,default);
        state.Phase=AwaitingDrop;
    }

    private static void Recover(ref ReachPickupState state, GameContext context, int duration)
    {
        int elapsed=++state.TicksInPhase;
        if(state.Phase==Released)
            state.Opening=elapsed==1 ? 1 : 1-Ease((double)(elapsed-1)/(duration-1));
        else
            state.Opening=.8*(1-Ease((double)elapsed/duration));

        PoseFingers(state,context);
        if(elapsed==duration)
            state.Phase=state.Phase==Released ? ReachPickupGame.Idle : ReachPickupGame.Held;
    }

    private static void PoseFingers(in ReachPickupState state, GameContext context) =>
        context.SetAnimationLayer(state.Rig,(uint)state.FingerLayer,state.OpenClip,1,
            time:state.OpenTime*state.Opening,loop:false,playing:false);

    private static void UpdatePrompt(in ReachPickupState state, GameContext context, in CarryPropSettings settings)
    {
        string text=state.Phase switch
        {
            ReachPickupGame.Idle => ReachPickupGame.AimedItem(state,context,settings.Camera).HasValue ? "Pick up" : "",
            ReachPickupGame.Reaching => "Reaching",
            ReachPickupGame.Returning => "",
            ReachPickupGame.BlockedReturning => "Reach blocked",
            ReachPickupGame.Held => "Drop",
            Blocked => "No room to drop",
            PreparingDrop or AwaitingDrop => "Releasing",
            _ => ""
        };
        var current=context.GetUi(settings.Prompt);
        bool visible=text.Length>0;
        if(current.Text!=text || current.Visible!=visible)context.SetUi(settings.Prompt,text,visible);
    }
}

[GameModule("poima.example.clearance-carry-props")]
public sealed class ClearanceCarryPropsGame : Game<ReachPickupState>,
    IIkClearanceGame, IItemAttachmentAimGame, IItemAttachmentPickupGame, IItemAttachmentReleaseGame,
    ITwoBoneIkOrientationGame, IMaskedAnimationGame, IInputActionsGame
{
    public override void Initialize(ref ReachPickupState state) { }
    public override void Tick(ref ReachPickupState state, GameContext context) =>
        CarryPropsGame.Update(ref state,context,ReachGuard.Preview);
}

[GameModule("poima.example.final-clearance-carry-props")]
public sealed class FinalClearanceCarryPropsGame : Game<ReachPickupState>,
    IIkClearanceGame, IItemAttachmentAimGame, IItemPickupClearanceGame, IItemAttachmentReleaseGame,
    ITwoBoneIkOrientationGame, IMaskedAnimationGame, IInputActionsGame
{
    public override void Initialize(ref ReachPickupState state) { }
    public override void Tick(ref ReachPickupState state, GameContext context) =>
        CarryPropsGame.Update(ref state,context,ReachGuard.Pickup);
}

[GameModule("poima.example.pose-fallback-carry-props")]
public sealed class PoseFallbackCarryPropsGame : Game<ReachPickupState>,
    IIkClearanceGame, IItemAttachmentAimGame, IItemPickupPoseGame, IItemAttachmentReleaseGame,
    ITwoBoneIkOrientationGame, IMaskedAnimationGame, IInputActionsGame
{
    public override void Initialize(ref ReachPickupState state) { }
    public override void Tick(ref ReachPickupState state, GameContext context) =>
        CarryPropsGame.Update(ref state,context,ReachGuard.PoseFallback);
}
