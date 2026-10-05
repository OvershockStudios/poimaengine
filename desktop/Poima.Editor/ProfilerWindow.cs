// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using System.Globalization;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public sealed class ProfilerWindow : Window
{
    private readonly ProfilerModel model;
    private readonly EditorModel editor;
    private readonly ProfilerTimeline timeline = new();
    private readonly TextBlock status = Note(""), message = Note(""), range = Note(""), selection = Note("");
    private readonly StackPanel summaries = new() { Spacing = 4 };
    private readonly ComboBox source = new() { ItemsSource = new[] { "All CPU sources", "native", "request", "player", "editor_poll", "editor_scene", "editor_game" }, SelectedIndex = 0, MinWidth = 160 };
    private readonly ComboBox metric = new() { ItemsSource = new[] { "Total", "Mean", "P95", "Maximum" }, SelectedIndex = 0, MinWidth = 100 };
    private JsonArray? displayed;
    private int page;
    private bool retired;
    private readonly DispatcherTimer statusTimer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private readonly Button record, stop, refresh, export;
    public ProfilerWindow(EditorModel editor, ProfilerModel model)
    {
        this.editor = editor; this.model = model;
        Title = "Profiler — Poima"; Width = 980; Height = 740; MinWidth = 650; MinHeight = 500;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = 12;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var root = new Grid { RowDefinitions = new("Auto,Auto,Auto,Auto,230,Auto,*,Auto"), Margin = new Thickness(14), RowSpacing = 8 };
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
        record = ActionButton("Record", "play", () => model.Record());
        stop = AsyncButton("Stop", "stop", model.StopAsync);
        refresh = AsyncButton("Refresh", "refresh", model.LoadAsync);
        export = AsyncButton("Export trace", "folder", ExportAsync);
        foreach (var action in new[] { record, stop, refresh, export }) actions.Children.Add(action);
        actions.Children.Add(ActionButton("Review current capture", "refresh", model.DiscardStart));
        Put(root, actions, 0); Put(root, status, 1); Put(root, message, 2);
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        controls.Children.Add(new TextBlock { Text = "CPU timeline", VerticalAlignment = VerticalAlignment.Center });
        AutomationProperties.SetName(source, "Profiler Source"); controls.Children.Add(source);
        controls.Children.Add(ActionButton("Previous events", "undo", () => { if (page > 0) --page; RefreshTimeline(); }));
        controls.Children.Add(ActionButton("Next events", "redo", () => { if ((page + 1) * ProfilerTimeline.Limit < Filtered().Count) ++page; RefreshTimeline(); }));
        controls.Children.Add(range); Put(root, controls, 3);
        var timelineScroll = new ScrollViewer { Content = timeline, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        Put(root, timelineScroll, 4);
        selection.TextWrapping = TextWrapping.Wrap; AutomationProperties.SetName(selection, "Profiler Selected event"); Put(root, selection, 5);
        var summaryPanel = new Grid { RowDefinitions = new("Auto,*"), RowSpacing = 5 };
        var sort = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        sort.Children.Add(new TextBlock { Text = "Cost summary", FontWeight = FontWeight.SemiBold, VerticalAlignment = VerticalAlignment.Center });
        AutomationProperties.SetName(metric, "Profiler Sort"); sort.Children.Add(metric);
        sort.Children.Add(Note("CPU and GPU totals are separate; nested spans overlap."));
        Put(summaryPanel, sort, 0); Put(summaryPanel, new ScrollViewer { Content = summaries }, 1); Put(root, summaryPanel, 6);
        Put(root, Note("GPU durations have no CPU-aligned start time. Storage is this recorder only; process memory and VRAM are not measured."), 7);
        Content = root;
        source.SelectionChanged += (_, _) => { page = 0; RefreshTimeline(); };
        metric.SelectionChanged += (_, _) => RefreshSummary();
        timeline.Selected += id => Guard(() => model.Select(id));
        model.Changed += Changed;
        statusTimer.Tick += (_, _) => { if (!retired && IsVisible && !model.Loading) { try { model.ObserveStatus(); } catch (Exception error) { statusTimer.Stop(); model.Fail(error); } } };
        statusTimer.Start();
        Closed += (_, _) => { retired = true; statusTimer.Stop(); model.Changed -= Changed; };
        Opened += (_, _) => { var screen = Screens.ScreenFromWindow(this); if (screen is not null) { Height = Math.Min(Height, screen.WorkingArea.Height / RenderScaling * .9); Width = Math.Min(Width, screen.WorkingArea.Width / RenderScaling * .95); } };
        Sync();
    }
    private static TextBlock Note(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#B4B6BF") };
    private static void Put(Grid grid, Control control, int row) { Grid.SetRow(control, row); grid.Children.Add(control); }
    private void Guard(Action action) { try { action(); } catch (Exception error) { model.Fail(error); } }
    private Button ActionButton(string name, string icon, Action action)
    {
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
        content.Children.Add(EditorIcons.Make(icon)); content.Children.Add(new TextBlock { Text = name, VerticalAlignment = VerticalAlignment.Center });
        var button = new Button { Content = content }; button.Classes.Add("editor-button");
        AutomationProperties.SetName(button, "Profiler " + name); ToolTip.SetTip(button, name);
        button.Click += (_, _) => Guard(action); return button;
    }
    private Button AsyncButton(string name, string icon, Func<Task> action) => ActionButton(name, icon, async () =>
    { try { await action(); } catch (Exception error) { if (!retired) model.Fail(error); } });
    private void Changed(object? sender, EventArgs args) { if (!retired) Sync(); }
    private void Sync()
    {
        var s = model.Status;
        status.Text = $"{model.State} · {s["count"] ?? 0}/{s["capacity"] ?? 0} events · {s["dropped"] ?? 0} dropped · recorder {Number(s["storage_bytes"]) / 1024:F1} KiB" +
            (model.Loading ? " · loading…" : model.Stale ? " · displayed capture is older; Refresh to review the current one" : "");
        AutomationProperties.SetName(status, "Profiler Status");
        message.Text = model.Error ?? ""; message.IsVisible = model.Error is not null; AutomationProperties.SetName(message, "Profiler Message");
        record.IsEnabled = !model.Loading && model.State != "recording"; stop.IsEnabled = !model.Loading && model.State is "recording" or "full";
        refresh.IsEnabled = !model.Loading; export.IsEnabled = !model.Loading && model.LoadedCapture is not null && !model.Stale;
        if (!ReferenceEquals(displayed, model.Events)) { displayed = model.Events; page = 0; RefreshTimeline(); RefreshSummary(); }
        var selected = model.Selected;
        selection.Text = selected is null ? "Select a colored CPU span to inspect its source, session and tick." :
            $"{selected["name"]} · {selected["source"]} · {Number(selected["duration_ns"]) / 1e6:F3} ms · start {Number(selected["start_ns"]) / 1e6:F3} ms" +
            $"\nTick {selected["tick"]?.ToString() ?? "—"} · session {selected["session"]?.ToString() ?? "none"} · parent {selected["parent"]} · failed {selected["failed"]}";
        timeline.SelectedId = selected?["id"]?.GetValue<long>(); timeline.InvalidateVisual();
    }
    private List<JsonObject> Filtered() => model.Events.OfType<JsonObject>().Where(value => value["kind"]?.GetValue<string>() == "cpu" &&
        (source.SelectedIndex <= 0 || value["source"]?.GetValue<string>() == source.SelectedItem?.ToString())).ToList();
    private void RefreshTimeline()
    {
        var values = Filtered(); page = Math.Clamp(page, 0, Math.Max(0, (values.Count - 1) / ProfilerTimeline.Limit));
        var shown = values.Skip(page * ProfilerTimeline.Limit).Take(ProfilerTimeline.Limit).ToList();
        range.Text = values.Count == 0 ? "No CPU spans" : $"{page * ProfilerTimeline.Limit + 1}–{page * ProfilerTimeline.Limit + shown.Count} / {values.Count}";
        timeline.Set(shown);
    }
    private void RefreshSummary()
    {
        summaries.Children.Clear();
        if (model.Summary?["groups"] is not JsonArray groups) { summaries.Children.Add(Note("Record and stop to inspect a capture.")); return; }
        double Cost(JsonObject group) => metric.SelectedIndex switch { 1 => Number(group["mean"]), 2 => Number(group["p95"]), 3 => Number(group["max"]), _ => Number(group["mean"]) * Number(group["samples"]) };
        foreach (var kind in new[] { "cpu", "gpu", "counter" })
        {
            var matching = groups.OfType<JsonObject>().Where(value => value["kind"]?.GetValue<string>() == kind);
            var values = kind == "counter"
                ? matching.OrderBy(value => value["unit"]?.GetValue<string>()).ThenByDescending(value => Number(value["last"])).ToList()
                : matching.OrderByDescending(Cost).ToList();
            if (values.Count == 0) continue;
            summaries.Children.Add(new TextBlock { Text = kind == "gpu" ? "GPU durations · not CPU-aligned" : kind == "counter" ? "Counters · latest value, grouped by unit" : "CPU durations · inclusive scopes overlap", FontWeight = FontWeight.SemiBold });
            foreach (var value in values.Take(256))
            {
                var nativeUnit = value["unit"]?.GetValue<string>() ?? "count";
                var scale = nativeUnit == "ns" ? 1e6 : 1;
                var unit = nativeUnit == "ns" ? "ms" : nativeUnit;
                var details = kind == "counter"
                    ? $"last {value["last"]} · min {value["min"]} · max {value["max"]} · mean {Number(value["mean"]):F3} {unit}"
                    : $"aggregate {Number(value["mean"]) * Number(value["samples"]) / scale:F3} · mean {Number(value["mean"]) / scale:F3} · p50 {Number(value["p50"]) / scale:F3} · p95 {Number(value["p95"]) / scale:F3} · p99 {Number(value["p99"]) / scale:F3} · max {Number(value["max"]) / scale:F3} {unit}";
                var row = Note($"{value["source"]} / {value["name"]}   ×{value["samples"]}\n" + details);
                summaries.Children.Add(row);
            }
            if (values.Count > 256) summaries.Children.Add(Note($"Showing the highest 256 of {values.Count} groups; Export includes the full capture."));
        }
    }
    internal static double Number(JsonNode? value) => value is null ? 0 : double.Parse(value.ToJsonString(), CultureInfo.InvariantCulture);
    private async Task ExportAsync()
    {
        var capture = model.LoadedCapture;
        var destination = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions { Title = "Export profiler trace (new file)", SuggestedFileName = "poima-trace.json", DefaultExtension = "json" });
        if (destination is null || retired) return;
        if (capture != model.LoadedCapture) throw new InvalidOperationException("Displayed capture changed while choosing an export path. Choose Export again.");
        var path = destination.TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local trace destination.");
        model.Export(path); editor.Note("Profiler trace exported: " + path);
    }
    internal JsonObject RenderForQualification(string destination, string projectRoot)
    {
        if (Program.Options.Script is null || !IsVisible || !IsMeasureValid || !IsArrangeValid) throw new InvalidOperationException("Profiler render requires a measured script-owned window.");
        var path = Path.GetFullPath(destination);
        var project = Path.GetFullPath(projectRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (path.StartsWith(project, StringComparison.OrdinalIgnoreCase) || Path.GetExtension(path).ToLowerInvariant() != ".png") throw new ArgumentException("Profiler render must be a new PNG outside the project.");
        var parent = new DirectoryInfo(Path.GetDirectoryName(path)!);
        if (!parent.Exists) throw new ArgumentException("Output directory must exist.");
        for (var check = parent; check is not null; check = check.Parent) if ((check.Attributes & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Output cannot traverse reparse points.");
        var width = Math.Ceiling(Bounds.Width * RenderScaling); var height = Math.Ceiling(Bounds.Height * RenderScaling);
        if (width is < 32 or > 4096 || height is < 32 or > 4096) throw new InvalidOperationException("Profiler render exceeds size budget.");
        using var image = new RenderTargetBitmap(new PixelSize((int)width, (int)height), new Vector(96 * RenderScaling, 96 * RenderScaling));
        image.Render(this); using var encoded = new MemoryStream(); image.Save(encoded, PngBitmapEncoderOptions.Default); encoded.Position = 0;
        using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None); encoded.CopyTo(output); output.Flush(true);
        return new() { ["kind"] = "avalonia_visual_content", ["path"] = path, ["width"] = (int)width, ["height"] = (int)height,
            ["qualification"] = "Attached Profiler visual content only; not an OS screenshot or physical display qualification." };
    }
}

internal sealed class ProfilerTimeline : Control
{
    internal const int Limit = 2048;
    private readonly List<(Rect Box, JsonObject Event)> hits = new();
    private List<JsonObject> events = new();
    private double begin, span = 1;
    private string[] lanes = Array.Empty<string>();
    public long? SelectedId { get; set; }
    public event Action<long>? Selected;
    private static readonly string[] Sources = { "native", "request", "player", "editor_poll", "editor_scene", "editor_game" };
    private static readonly string[] Colors = { "#70AAC9", "#C39CE0", "#8BC996", "#D4B379", "#6DA5E0", "#DF9485" };
    public ProfilerTimeline()
    {
        Height = 80; MinWidth = 350; Focusable = true;
        AutomationProperties.SetName(this, "Profiler CPU timeline");
        ToolTip.SetTip(this, "Click a CPU span, or focus this timeline and use Left/Right to select events in this page.");
        KeyDown += (_, e) =>
        {
            if (events.Count == 0 || e.Key is not (Key.Left or Key.Right)) return;
            var index = events.FindIndex(value => value["id"]!.GetValue<long>() == SelectedId);
            index = Math.Clamp(index + (e.Key == Key.Right ? 1 : -1), 0, events.Count - 1);
            Selected?.Invoke(events[index]["id"]!.GetValue<long>()); e.Handled = true;
        };
    }
    public void Set(List<JsonObject> values)
    {
        events = values;
        lanes = Sources.Where(source => values.Any(value => value["source"]?.GetValue<string>() == source)).ToArray();
        Height = Math.Max(80, lanes.Length * 56 + 24);
        begin = values.Count == 0 ? 0 : values.Min(value => ProfilerWindow.Number(value["start_ns"]));
        span = values.Count == 0 ? 1 : Math.Max(1, values.Max(value => ProfilerWindow.Number(value["start_ns"]) + ProfilerWindow.Number(value["duration_ns"])) - begin);
        InvalidateVisual();
    }
    public override void Render(DrawingContext context)
    {
        base.Render(context); hits.Clear(); context.FillRectangle(EditorTheme.Brush("#202126"), new Rect(Bounds.Size));
        var width = Math.Max(1, Bounds.Width - 120);
        var ancestors = events.ToDictionary(value => value["id"]!.GetValue<long>());
        for (var lane = 0; lane < lanes.Length; ++lane)
        {
            var label = new FormattedText(lanes[lane], CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Inter"), 11, EditorTheme.Brush("#CBCDD4"));
            context.DrawText(label, new Point(5, lane * 56 + 7));
            context.DrawLine(new Pen(EditorTheme.Brush("#363840")), new Point(116, lane * 56), new Point(Bounds.Width, lane * 56));
        }
        foreach (var value in events)
        {
            var lane = Array.IndexOf(lanes, value["source"]!.GetValue<string>()); if (lane < 0) continue;
            var depth = 0; var parent = value["parent"]?.GetValue<long>() ?? 0;
            for (var guard = 0; guard < 64 && ancestors.TryGetValue(parent, out var ancestor); ++guard)
            { if (ancestor["source"]?.GetValue<string>() == value["source"]?.GetValue<string>()) ++depth; var next = ancestor["parent"]?.GetValue<long>() ?? 0; if (next == parent) break; parent = next; }
            var x = 118 + (ProfilerWindow.Number(value["start_ns"]) - begin) / span * width;
            var w = Math.Max(2, ProfilerWindow.Number(value["duration_ns"]) / span * width);
            var rect = new Rect(x, lane * 56 + 5 + Math.Min(depth, 3) * 12, Math.Min(w, Math.Max(1, Bounds.Width - x)), 10);
            context.FillRectangle(EditorTheme.Brush(value["failed"]?.GetValue<bool>() == true ? "#E36C70" : Colors[Array.IndexOf(Sources, lanes[lane])]), rect);
            if (value["id"]!.GetValue<long>() == SelectedId) context.DrawRectangle(new Pen(Brushes.White, 2), rect);
            hits.Add((rect, value));
        }
        context.DrawText(new FormattedText($"{begin / 1e6:F3} ms → {(begin + span) / 1e6:F3} ms", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Inter"), 11, Brushes.LightGray), new Point(118, lanes.Length * 56 + 3));
    }
    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e); var point = e.GetPosition(this);
        var hit = hits.LastOrDefault(item => item.Box.Contains(point));
        if (hit.Event is not null) { Focus(); Selected?.Invoke(hit.Event["id"]!.GetValue<long>()); e.Handled = true; }
    }
}
