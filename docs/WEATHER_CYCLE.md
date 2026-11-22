# Weather and solar time package

`Poima.Weather` is an optional compiled C# package for a local weather forecast
and day/night cycle. It takes integer game time and authored settings. It returns
the sun direction, season, rain, cloud cover, wind, a wetness target and timed
lightning strikes. It can also measure accumulated rain across a fast-forward
interval by integrating the same rain rate used by the forecast.

Reference `packages/weather/Poima.Weather.csproj` from a Poima C# project. The
package targets .NET 10 and has no engine ABI or external dependency. A game can
use it from its compiled update, a headless replay or an editor preview. The game
owns the clock and applies the results to its scene, environment audio and water
simulation. Drive the visible sun with [solar lighting](#drive-the-sun-and-sky).
Route integrated rain and drying through native [surface wetness](SURFACE_WETNESS.md).
Sky scattering, snow and regional weather fronts are separate systems.

## Sample a day and its weather

Save the world time as an integer count of **game milliseconds**. The calendar
repeats after 365 days and accepts elapsed time through the final millisecond
of year index 9,999. Day of year starts at one. The package takes local solar
time, where 12:00 is solar noon. Clock acceleration belongs to the caller. For
example, compute elapsed world milliseconds from the gameplay tick and a fixed
integer ratio so pausing and replay retain the same time.

The solar model puts the northern spring equinox at noon on day 80 and uses
sinusoidal declination over the calendar year. `SolarClock.Sample` accepts
latitude from −90° through 90° and axial tilt from 0° through 90°. The supplied
default axial tilt is 23.44°. Southern seasons shift by half a year. The unit
vector points **toward** the sun in +Y-up world axes with north at −Z and east at
+X. Negate the vector if your light API takes the propagation direction.
Elevation and clockwise azimuth from north are in radians. At the zenith or
nadir, azimuth is defined as zero. [Solar geometry reference](https://pvpmc.sandia.gov/modeling-guide/1-weather-design-inputs/sun-position/basic-solar-position-models/)

```csharp
using Poima.Weather;

// Create the forecast once with authored settings and a world seed.
var schedule = new WeatherSchedule(seed: 42, slotSeconds: 3_600,
    transitionSeconds: 900, WeatherWeights.Temperate);

// Example state at 14:00 on day 80 of the first year.
long now = 79L * WeatherClock.MillisecondsPerDay + 14L * 3_600_000;
SolarSample sun = SolarClock.Sample(now, latitudeDegrees: 42);
WeatherSample forecast = schedule.Sample(now);
double rainRate = forecast.Conditions.RainMillimetersPerHour;
double rainDepth = schedule.RainDepthMillimeters(now - 10_000, now);
```

Each forecast slot lasts 60–86,400 game seconds. Relative integer mode weights
choose clear, overcast, rain or storm. A zero weight disables that mode.
`WeatherWeights.Temperate` assigns weights of 45, 30, 20 and 5 respectively.
The slot choice and per-mode variation come from the seed and slot index.
The previous slot's values blend into the current target over the first
`transitionSeconds` of each slot. Set that duration from zero through half the
slot length. The blend uses smoothstep, so nonzero transitions join with zero
slope at both ends. The first slot starts at its target.

Use `Conditions` for continuous values. `Kind` reports the current slot's
target mode while `PreviousKind` identifies the start of a transition. Rain
targets are 0 mm/h for clear and overcast, 4–10 mm/h for rain and 16–28 mm/h for
storm. Wind along +X and +Z is in metres per second. Cloud cover and wetness
targets are in [0, 1]. A game can use the wetness target as a material input and
the accumulated depth for its own saved surface or water state.

`RainDepthNanometers(quantumIndex)` integrates a complete game second and
rounds its result once to an integer nanometre. Index zero spans [0, 1000)
game milliseconds. The last complete index is 315,359,999,998. Feed the same
sequence of quanta at different clock speeds to preserve the same dose history.

## Drive the sun and sky

Give the world an enabled [procedural sky](LIGHTING.md#procedural-sky) linked to
an enabled authored directional Light. Its lux, linear tint and sky disk
radiance set the brightest reference. The sky palette sets the gradient at
full brightness. Declare `ISolarLightingGame` on the compiled game module.
Discover the `solar_lighting` section with `world.describe` and look for
`solar_lighting_v1` with a 584-byte gameplay service prefix.

Call `SetSolarLighting` once in a gameplay update with the game time and a
direction toward the sun. The following sketch uses one shared forecast and
clock. Tune the elevation transitions and gains for the game's art direction.

```csharp
using System.Numerics;
using Poima;
using Poima.Weather;

static float Fade(float value, float from, float to)
{
    float t = Math.Clamp((value - from) / (to - from), 0f, 1f);
    return t * t * (3f - 2f * t);
}

// Within Tick, using the same now value as rain and lightning.
var solar = SolarClock.Sample(now, latitudeDegrees: 42);
float elevation = (float)(solar.ElevationRadians * (180 / Math.PI));
float cloud = (float)forecast.Sample(now).Conditions.CloudCover;
float daylight = Fade(elevation, -9f, 8f);
float direct = Fade(elevation, 0f, 10f) * (1f - .75f * cloud);
float skyGain = .04f + .96f * daylight * (1f - .25f * cloud);
var sunTint = Vector3.Lerp(new(1, .4f, .18f), Vector3.One, Fade(elevation, 0f, 24f));
var ambient = Vector3.Lerp(new(.004f, .007f, .02f), new(.05f, .07f, .1f), daylight);
var toward = solar.Direction;
context.SetSolarLighting((ulong)now, new Vector3d(toward.X, toward.Y, toward.Z),
    direct, sunTint, skyGain, ambient);
```

`sun_gain` scales directional lux and sky disk radiance. `sun_tint` multiplies
the directional light's linear RGB and applies to its disk. `sky_gain` scales
the gradient and its baked diffuse and specular environment light. The sky
textures are reused as the game advances, and visible screen-space reflection
hits retain their authored specular response. The ambient RGB replaces the
authored diffuse fill. Both gains and tint channels are in [0, 1], and ambient
channels are in [0, 1,000,000]. The direction is finite and within 0.000001 of
unit length. The engine normalizes it for resolved lighting. Set the sun gain
to zero when the sun should disappear. Its shadow slots remain allocated.

`runtime.lighting` and runtime captures expose the last accepted sample as
`solar_lighting` alongside the resolved light, effective disk and ambient
values. The returned gradient colors describe the authored palette. Apply
the reported gain to get its effective radiance. A new session starts with
the authored lighting, then applies samples on successful ticks. A failed
batch restores the previous sample. A tick with no sample keeps the current
one, and save/load restores it before the next update. Save the clock and
forecast inputs in gameplay state so future samples continue from the same time.

## Collect deterministic events

Each storm slot has one independently timed strike. Its time falls after the
slot's weather transition and before the final quarter. The event strength is
in [0.65, 1]. There is no event-location model in the package. The caller can
place the effect and play lightning and thunder using its own scene policy.

```csharp
long lastProcessed = now - 10_000;
Span<LightningStrike> strikes = stackalloc LightningStrike[WeatherSchedule.MaximumQuerySlots];
int count = schedule.CollectLightning(lastProcessed, now, strikes);
foreach (LightningStrike strike in strikes[..count])
    Console.WriteLine($"Storm {strike.SlotIndex} at {strike.WorldMilliseconds} ms");
lastProcessed = now;
```

Save `lastProcessed` alongside the world time and the schedule settings. The
query includes strikes in `(lastProcessed, now]` and emits them in chronological
order. Use −1 when querying from the epoch. Advance the saved cursor after
applying the returned events. A query that spans more than 64 slots or has a
short output buffer fails before copying any output. Split longer fast-forward
intervals, retaining adjacent boundaries. Rain integration accepts up to 64
touched slots per call and uses `[start, end)` intervals.

Weather modes and strike times depend on integer calculations. Solar direction,
wind and interpolation use floating-point mathematics and are compared with
tolerances across platforms. Keep schedule settings fixed across a save and
restore to preserve the forecast replay.

## Run the compiled checks

```sh
dotnet run --project tests/WeatherCycle/Poima.WeatherCycle.Tests.csproj
```

The checks cover year wrap, solar elevation and direction, both hemispheres
and polar day and night. They also confirm seeded weather choices, smooth
state, equivalent rainfall across partitioned intervals, and lightning replay
with strict interval and buffer bounds.

The [solar lighting fixture](../tests/solar_lighting_contract.py) loads the
same weather-driven game through CoreCLR or Native AOT. It checks the
direction, gains and ambient fill at each tick, then compares complete saves
after rollback and fresh-process continuation. The Windows Vulkan route also
checks the rendered sky from a relocated export. Run the script with `--help`
for artifact and capture options.
