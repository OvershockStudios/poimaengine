using System.Numerics;
using System.Runtime.InteropServices;
using Poima;

namespace RelayDefense;

// All mutable authoritative values live here (native-owned, scalar/EntityId only).
[StructLayout(LayoutKind.Sequential)]
public struct RelayState
{
    public int Wave;        // 0 before the first spawn, then 1..3
    public int Kills;
    public int Ammo;
    public int CoreHealth;
    public int Won;
    public int Lost;
    public int Paused;
    public int Shots;       // accepted shots (each consumed one round)
    public int Breaches;
    public int Cooldown;    // ticks remaining before the next shot is accepted
    public int Hits;        // shots that damaged a drone
    public int Misses;      // shots that hit nothing
    public int CoverHits;   // shots stopped by cover/reactor/floor (non-drone collider)
    public int RejectedCooldown;
    public int RejectedEmpty;
    public int Reloads;
    public int SaveRequests;
    public int LoadRequests;
    public EntityId Drone1;
    public EntityId Drone2;
    public EntityId Drone3;
    public int Drone1Hp;
    public int Drone2Hp;
    public int Drone3Hp;
}

[GameModule("relay-defense.v1")]
public sealed class RelayDefenseGame : Game<RelayState>
{
    public const int MagazineSize = 6;
    public const int CooldownTicks = 12;
    public const int DroneHitPoints = 2;
    public const int StartingHealth = 3;
    public const int WaveCount = 3;
    public const int DronesPerWave = 3;
    public const string SaveSlot = "relay-checkpoint";

    // Lane geometry: drones spawn at SpawnZ and advance +Z to the reactor perimeter at BreachZ.
    const double SpawnZ = -40.0;
    const double BreachZ = 6.0;
    const double DroneY = 1.5;
    const double StepPerTick = 0.035;
    const double RayDistance = 120.0;
    static double LaneX(int lane) => lane switch { 0 => -6.0, 1 => 0.0, _ => 6.0 };

    static readonly EntityId Player = EntityId.Parse("7e1a0000000000000000000000000001");
    static readonly EntityId Camera = EntityId.Parse("7e1a0000000000000000000000000002");
    static readonly TemplateId DroneTemplate = TemplateId.Parse("7e1a00000000000000000000000000d0");
    static readonly UiId Hud = UiId.Parse("7e1a00000000000000000000000000a1");

    public override void Initialize(ref RelayState state)
    {
        state = default;
        state.Ammo = MagazineSize;
        state.CoreHealth = StartingHealth;
    }

    public override void Tick(ref RelayState state, GameContext context)
    {
        if (state.Paused != 0 || state.Won != 0 || state.Lost != 0)
        {
            RefreshHud(ref state, context);
            return;
        }

        if (state.Cooldown > 0) --state.Cooldown;

        // 1. Shots resolve against committed state (camera already has this tick's look applied).
        if (context.Pressed(Player, GameAction.Use)) Fire(ref state, context);

        // 2. Breaches for surviving drones, then 3. motion for the rest. A drone is never
        // moved and despawned in the same tick.
        AdvanceDrone(ref state, context, ref state.Drone1, ref state.Drone1Hp, 0);
        AdvanceDrone(ref state, context, ref state.Drone2, ref state.Drone2Hp, 1);
        AdvanceDrone(ref state, context, ref state.Drone3, ref state.Drone3Hp, 2);

        if (state.CoreHealth <= 0)
        {
            state.CoreHealth = 0;
            state.Lost = 1;
        }
        else if (state.Drone1 == default && state.Drone2 == default && state.Drone3 == default)
        {
            if (state.Wave < WaveCount) SpawnWave(ref state, context);
            else if (state.Kills >= WaveCount * DronesPerWave) state.Won = 1;
            else state.Lost = 1; // all waves exhausted without a full clear
        }

        RefreshHud(ref state, context);
    }

    static void Fire(ref RelayState state, GameContext context)
    {
        if (state.Cooldown > 0) { ++state.RejectedCooldown; return; }
        if (state.Ammo <= 0) { ++state.RejectedEmpty; return; }

        --state.Ammo;
        ++state.Shots;
        state.Cooldown = CooldownTicks;

        var eye = context.Get(Camera).Transform;
        Span<EntityId> ignore = [Player, Camera];
        var hit = context.Raycast(eye.Position, eye.Forward, RayDistance, ignore);
        if (hit is not { } h) { ++state.Misses; return; }

        if (Damage(ref state, context, h.Entity, ref state.Drone1, ref state.Drone1Hp) ||
            Damage(ref state, context, h.Entity, ref state.Drone2, ref state.Drone2Hp) ||
            Damage(ref state, context, h.Entity, ref state.Drone3, ref state.Drone3Hp))
        {
            ++state.Hits;
            return;
        }
        ++state.CoverHits;
    }

    static bool Damage(ref RelayState state, GameContext context, EntityId target, ref EntityId drone, ref int hp)
    {
        if (drone == default || drone != target) return false;
        if (--hp <= 0)
        {
            context.Despawn(drone);
            drone = default;
            hp = 0;
            ++state.Kills;
        }
        return true;
    }

    static void AdvanceDrone(ref RelayState state, GameContext context, ref EntityId drone, ref int hp, int lane)
    {
        if (drone == default) return;
        var position = context.Get(drone).Transform.Position;
        if (position.Z >= BreachZ - 1e-6)
        {
            context.Despawn(drone);
            drone = default;
            hp = 0;
            ++state.Breaches;
            --state.CoreHealth;
            return;
        }
        double z = Math.Min(position.Z + StepPerTick, BreachZ);
        context.MoveKinematic(drone, new Vector3d(LaneX(lane), DroneY, z), Quaternion.Identity, 1);
    }

    static void SpawnWave(ref RelayState state, GameContext context)
    {
        ++state.Wave;
        state.Drone1 = SpawnDrone(context, 0);
        state.Drone2 = SpawnDrone(context, 1);
        state.Drone3 = SpawnDrone(context, 2);
        state.Drone1Hp = state.Drone2Hp = state.Drone3Hp = DroneHitPoints;
    }

    static EntityId SpawnDrone(GameContext context, int lane)
    {
        var start = new Vector3d(LaneX(lane), DroneY, SpawnZ);
        var id = context.Spawn(DroneTemplate, new SpawnTransform(start, Quaternion.Identity, new Vector3d(1.2, 1.2, 1.2)));
        // Reserved IDs may receive motion on their creation tick.
        context.MoveKinematic(id, new Vector3d(start.X, start.Y, start.Z + StepPerTick), Quaternion.Identity, 1);
        return id;
    }

    public override void Control(ref RelayState state, ControlContext context)
    {
        switch (context.Action)
        {
            case "relay.reload":
                if (state.Won == 0 && state.Lost == 0)
                {
                    state.Ammo = MagazineSize;
                    ++state.Reloads;
                }
                break;
            case "relay.pause":
                state.Paused = 1;
                context.RequestPause();
                break;
            case "relay.resume":
                state.Paused = 0;
                context.RequestResume();
                break;
            case "relay.save":
                ++state.SaveRequests;
                context.RequestSave(SaveSlot);
                break;
            case "relay.load":
                ++state.LoadRequests;
                context.RequestLoad(SaveSlot);
                break;
            default:
                throw new InvalidOperationException($"Unhandled RELAY DEFENSE action '{context.Action}'.");
        }
        string text = HudText(in state);
        if (context.GetUi(Hud).Text != text) context.SetUi(Hud, text);
    }

    static void RefreshHud(ref RelayState state, GameContext context)
    {
        string text = HudText(in state);
        if (context.GetUi(Hud).Text != text) context.SetUi(Hud, text);
    }

    static string HudText(in RelayState state)
    {
        string status = state.Won != 0 ? "VICTORY" : state.Lost != 0 ? "REACTOR LOST" : state.Paused != 0 ? "PAUSED" : "DEFENDING";
        int live = (state.Drone1 != default ? 1 : 0) + (state.Drone2 != default ? 1 : 0) + (state.Drone3 != default ? 1 : 0);
        return $"RELAY DEFENSE | Wave {state.Wave}/{WaveCount} | Kills {state.Kills}/{WaveCount * DronesPerWave} | " +
               $"Ammo {state.Ammo}/{MagazineSize} | Reactor {state.CoreHealth}/{StartingHealth} | Drones {live} | {status}";
    }
}
