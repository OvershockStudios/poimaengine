// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public sealed class GameplayWindow : Window
{
    private readonly GameplayEditorModel model;
    private readonly EditorModel editor;
    private readonly TextBlock message = new() { TextWrapping = TextWrapping.Wrap };
    private bool forceClose;
    private bool retired;
    public GameplayWindow(EditorModel editor, GameplayEditorModel model)
    {
        this.editor = editor; this.model = model;
        Title = "C# Gameplay — Poima"; Width = 660; Height = 780; MinWidth = 440; MinHeight = 440;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = 12;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var root = new StackPanel { Spacing = 8, Margin = new Thickness(14) };
        root.Children.Add(new TextBlock { Text = "C# Gameplay", FontSize = 18 });
        root.Children.Add(new TextBlock { Text = "Use a prebuilt C# assembly. Build changes in your IDE, pause Play, then reload. Launch configuration lasts for this editor session.", TextWrapping = TextWrapping.Wrap });
        var configStatus = Readout("Configuration status"); root.Children.Add(configStatus);
        var config = new StackPanel { Spacing = 5 };
        var controls = new Dictionary<string, TextBox>(StringComparer.Ordinal);
        foreach (var name in new[] { "Assembly", "Game type", "Initial values JSON", "Host runtime", "Managed bridge" })
        {
            var control = Field(name, model.Configuration[name]); controls.Add(name, control);
            Observe(control, text => model.SetConfiguration(name, text));
            if (name is "Host runtime" or "Managed bridge") continue;
            var row = FieldRow(name, control);
            if (name == "Assembly")
            {
                var browse = MakeButton("Browse assembly", () => _ = Browse(control));
                row.Children.Add(browse);
            }
            config.Children.Add(row);
        }
        var advanced = new StackPanel { Spacing = 5 };
        foreach (var name in new[] { "Host runtime", "Managed bridge" }) advanced.Children.Add(FieldRow(name, controls[name]));
        config.Children.Add(new Expander { Header = "Advanced runtime paths", Content = advanced });
        var configActions = new WrapPanel { Orientation = Orientation.Horizontal };
        var configure = MakeButton("Configure launch", () => model.Configure());
        var clear = MakeButton("Clear launch", () => model.Configure(true));
        configActions.Children.Add(configure); configActions.Children.Add(MakeButton("Revert configuration", model.RevertConfiguration)); configActions.Children.Add(clear);
        config.Children.Add(configActions);
        var configurationSection = new Expander { Header = "Launch configuration", Content = config, IsExpanded = true };
        root.Children.Add(configurationSection);
        root.Children.Add(new Separator());
        var liveStatus = Readout("Live status"); root.Children.Add(liveStatus);
        var live = new StackPanel { Spacing = 5 }; root.Children.Add(live);
        var reloadPath = Field("Reload assembly", model.ReloadAssembly);
        Observe(reloadPath, model.SetReloadAssembly);
        var reloadRow = FieldRow("Reload assembly · launch profile unchanged", reloadPath);
        reloadRow.Children.Add(MakeButton("Browse reload assembly", () => _ = Browse(reloadPath))); root.Children.Add(reloadRow);
        var liveActions = new WrapPanel { Orientation = Orientation.Horizontal };
        var refresh = MakeButton("Revert / refresh values", model.RefreshValues);
        var apply = MakeButton("Apply live values", model.ApplyValues);
        var reload = MakeButton("Reload assembly retaining state", model.Reload);
        liveActions.Children.Add(refresh); liveActions.Children.Add(apply); liveActions.Children.Add(reload); root.Children.Add(liveActions);
        root.Children.Add(new TextBlock { Text = "Live edits do not change launch values or authored objects. Stop discards unsaved runtime state; use Window > Saves for checkpoints. Reload preserves compatible fields and initializes new fields.", TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#A0A0A5") });
        AutomationProperties.SetName(message, "Gameplay message"); root.Children.Add(message);
        Content = new ScrollViewer { Content = root, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        long version = -1;
        bool? wasStopped = null;
        void Sync()
        {
            if (wasStopped != model.Stopped) { configurationSection.IsExpanded = model.Stopped; wasStopped = model.Stopped; }
            configStatus.Text = $"Launch generation {model.Generation}" + (model.Configured ? " · configured" : " · no gameplay") +
                (model.ConfigConflict ? " · changed externally; revert configuration" : model.ConfigDirty ? " · unapplied changes" : "");
            var runtime = editor.Host.State["gameplay"]?["runtime"];
            var loaded = runtime?["module"];
            liveStatus.Text = runtime is null ? "No running simulation." :
                $"{editor.PlaybackState} · tick {runtime["tick"]} · gameplay revision {runtime["revision"]}\n" +
                (loaded is null ? "No gameplay loaded. Load live values, then Reload assembly." : $"{loaded["identity"]}\nImage SHA-256: {loaded["assembly_sha256"]}") +
                (model.LiveConflict ? "\nObserved values are stale. Refresh before applying." : "") +
                (model.ValuesDirty ? "\nUnapplied live values" : "");
            if (model.Observation is { } observed)
                liveStatus.Text += $"\nObserved session {observed["session_id"]}\nObserved tick {observed["tick"]} · gameplay revision {observed["revision"]}";
            configure.IsEnabled = clear.IsEnabled = model.Stopped;
            foreach (var control in controls.Values) control.IsEnabled = model.Stopped;
            apply.IsEnabled = reload.IsEnabled = model.Paused;
            message.Text = model.Error ?? "";
            if (version != model.DraftVersion)
            {
                version = model.DraftVersion;
                foreach (var (name, control) in controls) control.Text = model.Configuration[name];
                reloadPath.Text = model.ReloadAssembly;
                live.Children.Clear();
                foreach (var item in model.Fields)
                {
                    var name = item!["name"]!.GetValue<string>(); var kind = item["kind"]!.GetValue<string>();
                    var field = Field("Value " + name, model.Values[name]); var draft = model.DraftVersion;
                    field.BorderBrush = EditorTheme.Brush(model.Invalid(name) ? "#BA6B60" : "#191919");
                    Observe(field, text =>
                    {
                        if (draft != model.DraftVersion) return;
                        model.SetValue(name, text); field.BorderBrush = EditorTheme.Brush(model.Invalid(name) ? "#BA6B60" : "#191919");
                    });
                    live.Children.Add(FieldRow(name + " · " + kind, field));
                }
            }
        }
        EventHandler changed = (_, _) => Sync();
        model.Changed += changed; editor.PlaybackChanged += changed;
        Closed += (_, _) => { retired = true; model.Changed -= changed; editor.PlaybackChanged -= changed; };
        Closing += (_, args) =>
        {
            if (!forceClose && model.Dirty)
            { args.Cancel = true; message.Text = "Apply or explicitly revert Gameplay drafts before closing."; }
        };
        Opened += (_, _) =>
        {
            var screen = Screens.ScreenFromWindow(this);
            if (screen is not null) Height = Math.Min(Height, screen.WorkingArea.Height / RenderScaling * .9);
        };
        Sync();
    }
    private static TextBox Readout(string name)
    {
        var text = new TextBox { IsReadOnly = true, TextWrapping = TextWrapping.Wrap, FontSize = 12 };
        AutomationProperties.SetName(text, "Gameplay " + name); return text;
    }
    private static TextBox Field(string name, string value)
    {
        var field = new TextBox { Text = value, FontSize = 12, MinWidth = 80 };
        AutomationProperties.SetName(field, "Gameplay " + name); return field;
    }
    private static StackPanel FieldRow(string name, Control field)
    {
        var panel = new StackPanel { Spacing = 3 };
        panel.Children.Add(new TextBlock { Text = name, TextWrapping = TextWrapping.Wrap }); panel.Children.Add(field); return panel;
    }
    private void Observe(TextBox control, Action<string> edit)
    {
        var shown = control.Text ?? "";
        control.TextChanged += (_, _) => { if (retired) return; var text = control.Text ?? ""; if (text == shown) return; shown = text; edit(text); };
    }
    private Button MakeButton(string label, Action action)
    {
        var button = new Button { Content = label, Margin = new Thickness(0, 3, 6, 3) }; button.Classes.Add("editor-button");
        AutomationProperties.SetName(button, "Gameplay " + label); ToolTip.SetTip(button, label);
        button.Click += (_, _) => { try { action(); } catch (Exception error) { model.Fail(error); } }; return button;
    }
    private async Task Browse(TextBox target)
    {
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = "Select compiled C# assembly", AllowMultiple = false,
                FileTypeFilter = new[] { new FilePickerFileType("C# assemblies") { Patterns = new[] { "*.dll" } } } });
            if (files.Count == 1 && files[0].TryGetLocalPath() is string path && IsVisible) target.Text = path;
        }
        catch (Exception error) { model.Fail(error); }
    }
    public void CloseForOwner() { forceClose = true; Close(); }
    internal void ScrollForQualification(string position)
    {
        if (Program.Options.Script is null || Content is not ScrollViewer scroll)
            throw new InvalidOperationException("Gameplay visual qualification requires an explicit script.");
        if (position == "top") scroll.ScrollToHome();
        else if (position == "bottom") scroll.ScrollToEnd();
        else throw new ArgumentException("Gameplay scroll position must be top or bottom.");
    }
    internal JsonObject RenderForQualification(string destination, string projectRoot)
    {
        if (Program.Options.Script is null || !IsVisible || !IsMeasureValid || !IsArrangeValid)
            throw new InvalidOperationException("Gameplay visual qualification requires a visible, measured script-owned tool window.");
        var path = Path.GetFullPath(destination);
        var project = Path.GetFullPath(projectRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (path.StartsWith(project, StringComparison.OrdinalIgnoreCase) || Path.GetExtension(path).ToLowerInvariant() != ".png")
            throw new ArgumentException("Gameplay visual output must be a new PNG outside the project directory.");
        var parent = new DirectoryInfo(Path.GetDirectoryName(path)!);
        if (!parent.Exists || File.Exists(path) || Directory.Exists(path)) throw new ArgumentException("Gameplay visual output requires a new file in an existing directory.");
        // Reject aliases before the containment decision can be bypassed by a
        // symlink or Windows junction. Never replace an existing output.
        for (var check = parent; check is not null; check = check.Parent)
            if ((check.Attributes & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Gameplay visual output cannot traverse a reparse point.");
        var width = Math.Ceiling(Bounds.Width * RenderScaling); var height = Math.Ceiling(Bounds.Height * RenderScaling);
        if (!double.IsFinite(width) || !double.IsFinite(height) || width is < 32 or > 4096 || height is < 32 or > 4096 || width * height > 16777216)
            throw new InvalidOperationException("Gameplay visual dimensions exceed the qualification budget.");
        // Render the actual attached window visual, including its normal
        // background. This does not include OS chrome, a screen or compositor.
        using var image = new RenderTargetBitmap(new PixelSize((int)width, (int)height), new Vector(96 * RenderScaling, 96 * RenderScaling));
        image.Render(this);
        using var encoded = new MemoryStream(); image.Save(encoded); encoded.Position = 0;
        using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        { encoded.CopyTo(output); output.Flush(true); }
        var scroll = Content as ScrollViewer;
        return new JsonObject { ["kind"] = "avalonia_visual_content", ["path"] = path,
            ["width"] = (int)width, ["height"] = (int)height, ["scale"] = RenderScaling,
            ["width_dip"] = Bounds.Width, ["height_dip"] = Bounds.Height, ["scroll_y"] = scroll?.Offset.Y ?? 0,
            ["qualification"] = "Attached Gameplay client visual only; not an OS screenshot, display/compositor or physical-input qualification." };
    }
}
