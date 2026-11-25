// SPDX-License-Identifier: Apache-2.0
using Poima.Weather;

const long Day = WeatherClock.MillisecondsPerDay;
const long Year = Day * WeatherClock.DaysPerYear;
const long Slot = 60_000;

void Assert(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

void Near(double actual, double expected, double tolerance = 1e-10)
{
    Assert(double.IsFinite(actual) && Math.Abs(actual - expected) <= tolerance,
        $"Expected {expected:R}, observed {actual:R} with tolerance {tolerance:R}.");
}

void Reject<TException>(Action action) where TException : Exception
{
    try { action(); }
    catch (TException) { return; }
    throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
}

long At(int day, int hour) => (day - 1) * Day + hour * (Day / 24);

Assert(WeatherClock.DateAt(0) == new GameDate(0, 1, 0), "Calendar epoch differs.");
Assert(WeatherClock.DateAt(Year - 1) == new GameDate(0, 365, (int)Day - 1), "Calendar year end differs.");
Assert(WeatherClock.DateAt(Year) == new GameDate(1, 1, 0), "Calendar year did not wrap.");
Assert(WeatherClock.DateAt(WeatherClock.MaximumTimeMilliseconds).YearIndex == 9_999, "Calendar upper bound differs.");
Reject<ArgumentOutOfRangeException>(() => WeatherClock.DateAt(-1));
Reject<ArgumentOutOfRangeException>(() => WeatherClock.DateAt(WeatherClock.MaximumTimeMilliseconds + 1));
Reject<ArgumentOutOfRangeException>(() => SolarClock.Sample(0, double.NaN));
Reject<ArgumentOutOfRangeException>(() => SolarClock.Sample(0, 90.001));
Reject<ArgumentOutOfRangeException>(() => SolarClock.Sample(0, 0, -1));

var east = SolarClock.Sample(At(80, 6), 0, axialTiltDegrees: 0);
var west = SolarClock.Sample(At(80, 18), 0, axialTiltDegrees: 0);
Near(east.Direction.X, 1);
Near(east.ElevationRadians, 0);
Near(east.AzimuthRadians, Math.PI / 2);
Near(west.Direction.X, -1);
Near(west.AzimuthRadians, Math.PI * 1.5);
var equinoxNoon = SolarClock.Sample(At(80, 12), 45);
Near(equinoxNoon.ElevationRadians, Math.PI / 4);
Near(equinoxNoon.AzimuthRadians, Math.PI);
Assert(equinoxNoon.IsAboveHorizon && equinoxNoon.Season == Season.Spring, "Northern equinox differs.");
Assert(!SolarClock.Sample(At(80, 0), 45).IsAboveHorizon, "Northern equinox midnight differs.");
Assert(SolarClock.Sample(At(80, 12), -45).Season == Season.Autumn, "Southern season differs.");
var northSummer = SolarClock.Sample(At(172, 0), 90);
var northWinter = SolarClock.Sample(At(355, 12), 90);
Assert(northSummer.IsAboveHorizon && northSummer.Season == Season.Summer, "Arctic summer differs.");
Assert(!northWinter.IsAboveHorizon && northWinter.Season == Season.Winter, "Arctic winter differs.");
Near(northSummer.ElevationRadians, 23.44 * Math.PI / 180, 1e-4);
Near(northWinter.ElevationRadians, -23.44 * Math.PI / 180, 1e-4);
foreach (double latitude in new[] { -90.0, -45, 0, 45, 90 })
{
    var now = SolarClock.Sample(At(127, 17), latitude);
    var nextYear = SolarClock.Sample(At(127, 17) + Year, latitude);
    var (x, y, z) = now.Direction;
    Near(x * x + y * y + z * z, 1);
    Assert(now.Direction == nextYear.Direction, "The solar schedule should repeat each year.");
}

var schedule = new WeatherSchedule(42, 60, 12, WeatherWeights.Temperate);
WeatherKind[] expectedModes = [WeatherKind.Clear, WeatherKind.Clear, WeatherKind.Rain, WeatherKind.Overcast,
    WeatherKind.Rain, WeatherKind.Overcast, WeatherKind.Clear, WeatherKind.Overcast, WeatherKind.Clear, WeatherKind.Clear];
for (int i = 0; i < expectedModes.Length; ++i)
    Assert(schedule.Sample(i * Slot).Kind == expectedModes[i], "Seeded weather sequence differs.");

var beforeRain = schedule.Sample(2 * Slot - 1);
var rainStart = schedule.Sample(2 * Slot);
var midBlend = schedule.Sample(2 * Slot + 6_000);
var steadyRain = schedule.Sample(2 * Slot + 12_000);
Assert(beforeRain.Conditions == rainStart.Conditions && rainStart.Transition == 0,
    "The rendered values should meet continuously at a slot boundary.");
Assert(rainStart.Kind == WeatherKind.Rain && rainStart.PreviousKind == WeatherKind.Clear,
    "The transition lost its source and target modes.");
Near(midBlend.Transition, 0.5);
Near(midBlend.Conditions.RainMillimetersPerHour, steadyRain.Conditions.RainMillimetersPerHour / 2);
Near(midBlend.Conditions.WetnessTarget, 0.325);
Near(midBlend.Conditions.WindX, (beforeRain.Conditions.WindX + steadyRain.Conditions.WindX) / 2);
Assert(steadyRain.Transition == 1 && steadyRain.Conditions.RainMillimetersPerHour is >= 4 and <= 10,
    "The rain rates are in mm/h.");

long firstRainStart = 2 * Slot;
Near(schedule.RainDepthMillimeters(firstRainStart, firstRainStart + 6_000),
    0.09375 * 12_000 * steadyRain.Conditions.RainMillimetersPerHour / 3_600_000);
Near(schedule.RainDepthMillimeters(firstRainStart, firstRainStart + 24_000),
    18_000 * steadyRain.Conditions.RainMillimetersPerHour / 3_600_000);
double skipped = schedule.RainDepthMillimeters(0, 32 * Slot);
double stepped = 0;
for (long start = 0; start < 32 * Slot; start += 250)
    stepped += schedule.RainDepthMillimeters(start, start + 250);
Assert(skipped > 0, "The rainfall test saw no rain.");
Near(skipped, stepped);
Near(schedule.RainDepthMillimeters(500, 500), 0);
Reject<ArgumentOutOfRangeException>(() => schedule.RainDepthMillimeters(500, 499));
Reject<ArgumentOutOfRangeException>(() => schedule.RainDepthMillimeters(0, 64 * Slot + 1));

var strikes = new LightningStrike[32];
int strikeCount = schedule.CollectLightning(-1, 32 * Slot - 1, strikes);
Assert(strikeCount == 3, "The seeded lightning count differs.");
Assert(strikes.Take(strikeCount).Select(strike => (strike.SlotIndex, strike.WorldMilliseconds))
    .SequenceEqual(new[] { (10L, 635_682L), (16L, 991_661L), (27L, 1_659_984L) }), "The seeded lightning times differ.");
var resumed = new WeatherSchedule(42, 60, 12, WeatherWeights.Temperate);
var collected = new LightningStrike[strikeCount];
int firstCount = resumed.CollectLightning(-1, 991_661, collected);
int secondCount = resumed.CollectLightning(991_661, 32 * Slot - 1, collected.AsSpan(firstCount));
Assert(firstCount == 2 && secondCount == 1 && collected.SequenceEqual(strikes.Take(strikeCount)),
    "Collecting across a saved lightning cursor must not skip or duplicate events.");
Assert(resumed.CollectLightning(635_682, 635_682, []) == 0, "An empty time interval must be empty.");
Assert(resumed.CollectLightning(635_681, 635_682, new LightningStrike[1]) == 1,
    "A strike at the inclusive end must be reported.");
var tooShort = new[] { new LightningStrike(9, 10, 11) };
Reject<ArgumentException>(() => schedule.CollectLightning(-1, 32 * Slot - 1, tooShort));
Assert(tooShort[0] == new LightningStrike(9, 10, 11), "A capacity error changed the output.");
Reject<ArgumentOutOfRangeException>(() => schedule.CollectLightning(-2, 15, strikes));
Reject<ArgumentOutOfRangeException>(() => schedule.CollectLightning(-1, 64 * Slot, strikes));
Reject<ArgumentOutOfRangeException>(() => new WeatherSchedule(0, 59, 0, WeatherWeights.Temperate));
Reject<ArgumentOutOfRangeException>(() => new WeatherSchedule(0, 60, 31, WeatherWeights.Temperate));
Reject<ArgumentException>(() => new WeatherSchedule(0, 60, 0, default));

var clear = new WeatherSchedule(6, 60, 0, new WeatherWeights(1, 0, 0, 0));
Assert(clear.CollectLightning(-1, 32 * Slot - 1, []) == 0, "Clear weather must have no lightning.");
Near(clear.RainDepthMillimeters(0, 32 * Slot), 0);
var storm = new WeatherSchedule(6, 60, 30, new WeatherWeights(0, 0, 0, 1));
Assert(storm.CollectLightning(-1, 64 * Slot - 1, new LightningStrike[64]) == 64, "A full bounded batch must fit.");
Assert(storm.CollectLightning(-1, Slot - 1, strikes) == 1 && strikes[0].WorldMilliseconds is >= 30_000 and < 45_000
    && strikes[0].Strength is >= 0.65 and <= 1, "A configured storm strike must stay in its stable interval.");
var late = storm.Sample(WeatherClock.MaximumTimeMilliseconds);
Assert(late.SlotIndex > uint.MaxValue && late.Kind == WeatherKind.Storm && late.Conditions.CloudCover is >= 0.92 and <= 1,
    "Large world time must preserve its slot address.");

// Independent integrals of the first clear-to-rain transition, the steady
// rain, and the transition back to overcast, rounded per complete second.
(long Index, uint Depth)[] expectedQuanta =
[
    (119, 0), (120, 13), (121, 86), (125, 848), (131, 1924), (132, 1937),
    (179, 1937), (180, 1924), (191, 13), (192, 0)
];
foreach (var (index, depth) in expectedQuanta)
    Assert(schedule.RainDepthNanometers(index) == depth, $"Rain quantum {index} differs.");
foreach (var (index, depth) in expectedQuanta.Reverse())
    Assert(resumed.RainDepthNanometers(index) == depth, $"Recreated rain quantum {index} differs.");
long roundedRain = 0;
for (long index = 120; index < 192; ++index)
    roundedRain += schedule.RainDepthNanometers(index);
Assert(roundedRain == 116_220, "Fixed one-second rounding differs.");

long lastQuantum = WeatherRainQuanta.MaximumQuantumIndex;
var steadyStorm = new WeatherSchedule(6, 60, 0, new WeatherWeights(0, 0, 0, 1));
Assert(lastQuantum == 315_359_999_998L && steadyStorm.RainDepthNanometers(lastQuantum) == 5995,
    "The last complete quantum must preserve the full interval and 64-bit address.");
Reject<ArgumentOutOfRangeException>(() => schedule.RainDepthNanometers(-1));
Reject<ArgumentOutOfRangeException>(() => schedule.RainDepthNanometers(lastQuantum + 1));
Reject<ArgumentOutOfRangeException>(() => schedule.RainDepthNanometers(long.MaxValue));
Reject<ArgumentNullException>(() => WeatherRainQuanta.RainDepthNanometers(null!, 0));

SeasonalTemperature[] normals = [new(12, 8), new(24, 19), new(9, 6), new(-6, -8)];
var northClimate = new SeasonalClimate(46, normals[0], normals[1], normals[2], normals[3]);
var southClimate = new SeasonalClimate(-46, normals[0], normals[1], normals[2], normals[3]);
long equinox = At(80, 12);
for (int phase = 0; phase < 4; ++phase)
{
    long midpoint = equinox + Year / 8 + phase * Year / 4;
    SeasonalClimateSample north = northClimate.Sample(midpoint);
    SeasonalClimateSample south = southClimate.Sample(midpoint);
    Assert(north.Solar.Season == (Season)phase && north.Temperature == normals[phase],
        "Northern seasonal midpoint failed to reach its authored normal.");
    int southPhase = (phase + 2) % 4;
    Assert(south.Solar.Season == (Season)southPhase && south.Temperature == normals[southPhase],
        "Southern seasonal midpoint did not shift by half a year.");
    SeasonalClimateSample nextYear = northClimate.Sample(midpoint + Year);
    Assert(north.Temperature == nextYear.Temperature && north.SnowFraction == nextYear.SnowFraction,
        "Seasonal climate did not repeat on the 365-day clock.");
}
Near(northClimate.Sample(equinox).Temperature.AirCelsius, 3);
Near(northClimate.Sample(equinox).Temperature.WetBulbCelsius, 0);
Near(northClimate.Sample(Year - 1).Temperature.AirCelsius,
    northClimate.Sample(Year).Temperature.AirCelsius, 1e-6);
Near(northClimate.Sample(equinox - 1).SnowFraction,
    northClimate.Sample(equinox + 1).SnowFraction, 1e-6);

SeasonalClimate Uniform(SeasonalTemperature temperature, double swing = 0) =>
    new(0, temperature, temperature, temperature, temperature, dailyAmplitudeCelsius: swing);

var daily = Uniform(new(10, 8), 3);
Assert(daily.Sample(At(80, 15)).Temperature == new SeasonalTemperature(13, 11), "Afternoon peak differs.");
Assert(daily.Sample(At(80, 3)).Temperature == new SeasonalTemperature(7, 5), "Overnight minimum differs.");
Near(Uniform(new(2, -1)).Sample(0).SnowFraction, 1);
Near(Uniform(new(3, 2)).Sample(0).SnowFraction, 0);
Near(Uniform(new(3, .5)).Sample(0).SnowFraction, .5);
Assert(Uniform(new(3, .5)).Partition(schedule, 132) == new PrecipitationPhaseDepth(968, 969),
    "A half-quantum must round the snow once and keep the liquid remainder.");
Assert(Uniform(new(-5, -6)).Partition(steadyStorm, lastQuantum) == new PrecipitationPhaseDepth(0, 5995),
    "The final supported quantum must conserve snow water equivalent.");
Assert(Uniform(new(26, 19)).Partition(steadyStorm, lastQuantum) == new PrecipitationPhaseDepth(5995, 0),
    "A warm interval must keep the existing rain depth.");
Assert(northClimate.Partition(clear, 0) == default, "Dry slots must stay dry.");

var phaseQuanta = new Dictionary<long, PrecipitationPhaseDepth>();
for (long quantum = equinox / 1000 - 32; quantum < equinox / 1000 + 32; ++quantum)
{
    PrecipitationPhaseDepth phase = northClimate.Partition(steadyStorm, quantum);
    Assert(phase.TotalNanometers == steadyStorm.RainDepthNanometers(quantum), "A phase split lost precipitation.");
    Assert(phase.RainNanometers > 0 && phase.SnowWaterEquivalentNanometers > 0, "Expected mixed precipitation.");
    phaseQuanta.Add(quantum, phase);
}
var freshClimate = new SeasonalClimate(46, normals[0], normals[1], normals[2], normals[3]);
foreach (var (quantum, phase) in phaseQuanta.Reverse())
    Assert(freshClimate.Partition(steadyStorm, quantum) == phase, "Reopening a climate changed a completed quantum.");
Reject<ArgumentOutOfRangeException>(() => northClimate.Partition(schedule, -1));
Reject<ArgumentOutOfRangeException>(() => northClimate.Partition(schedule, lastQuantum + 1));
Reject<ArgumentNullException>(() => northClimate.Partition(null!, 0));
Reject<ArgumentOutOfRangeException>(() => Uniform(new(2, 3)));
Reject<ArgumentOutOfRangeException>(() => Uniform(new(double.PositiveInfinity, -3)));
Reject<ArgumentOutOfRangeException>(() => Uniform(new(-3, -5), double.NaN));
Reject<ArgumentOutOfRangeException>(() => new SeasonalClimate(0, normals[0], normals[1], normals[2], normals[3],
    fullSnowWetBulbCelsius: 2, fullRainWetBulbCelsius: 1));

Console.WriteLine("Weather clock verified calendar wrap, solar geometry, seeded transitions, rain accumulation, lightning replay, seasonal climate and conserved rain/snow quanta.");
