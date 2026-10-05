// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public sealed class AudioWindow : Window
{
    private readonly AudioEditorModel model;
    private readonly CheckBox enabled = new() { Content = "Enable Game audio" }, muted = new() { Content = "Mute" };
    private readonly TextBox volume = new() { MinWidth = 90, MaxWidth = 160, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly TextBlock status = Note(""), message = Note(""), diagnostics = Note("");
    private readonly StackPanel pending = new() { Orientation = Orientation.Horizontal, Spacing = 6 };
    private bool syncing, retired, ownerClosing;
    public AudioWindow(AudioEditorModel model)
    {
        this.model = model;
        Title = "Audio — Poima"; Width = 540; Height = 490; MinWidth = 410; MinHeight = 380;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = 12;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var root = new StackPanel { Margin = new Thickness(16), Spacing = 12 };
        root.Children.Add(Note("Hear the running game from its Game camera. Enabling output does not start sounds."));
        AutomationProperties.SetName(enabled, "Audio Enable"); AutomationProperties.SetName(muted, "Audio Mute");
        AutomationProperties.SetName(volume, "Audio Volume");
        root.Children.Add(enabled); root.Children.Add(muted);
        root.Children.Add(new TextBlock { Text = "Volume · 0 to 1" }); root.Children.Add(volume);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        actions.Children.Add(Button("Apply", model.Apply)); actions.Children.Add(Button("Reload", model.Reload));
        actions.Children.Add(Button("Retry output", model.RetryDevice)); root.Children.Add(actions);
        pending.Children.Add(Button("Retry request", model.RetryPending)); pending.Children.Add(Button("Dismiss request", model.DismissPending)); root.Children.Add(pending);
        AutomationProperties.SetName(status, "Audio Status"); AutomationProperties.SetName(message, "Audio Message");
        root.Children.Add(status); root.Children.Add(message);
        AutomationProperties.SetName(diagnostics, "Audio Details");
        root.Children.Add(new Expander { Header = "Details", Content = diagnostics });
        Content = new ScrollViewer { Content = root };
        enabled.IsCheckedChanged += (_, _) => { if (!syncing) model.SetEnabled(enabled.IsChecked == true); };
        muted.IsCheckedChanged += (_, _) => { if (!syncing) model.SetMuted(muted.IsChecked == true); };
        volume.TextChanged += (_, _) => { if (!syncing) model.SetVolume(volume.Text ?? ""); };
        model.Changed += Changed;
        Closing += (_, e) => { if (!ownerClosing && model.Dirty) { e.Cancel = true; model.Fail(new InvalidOperationException("Apply or reload Audio changes and resolve pending requests before closing.")); } };
        Closed += (_, _) => { retired = true; model.Changed -= Changed; };
        Opened += (_, _) => { var screen = Screens.ScreenFromWindow(this); if (screen is not null) { Height = Math.Min(Height, screen.WorkingArea.Height / RenderScaling * .9); Width = Math.Min(Width, screen.WorkingArea.Width / RenderScaling * .95); } };
        Sync();
    }
    private static TextBlock Note(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#B4B6BF") };
    private Button Button(string label, Action action)
    {
        var button = new Button { Content = label }; button.Classes.Add("editor-button"); AutomationProperties.SetName(button, "Audio " + label);
        button.Click += (_, _) => { try { action(); } catch (Exception error) { model.Fail(error); } }; return button;
    }
    private void Changed(object? sender, EventArgs args) { if (!retired) Sync(); }
    private void Sync()
    {
        syncing = true;
        try
        {
            enabled.IsChecked = model.Enabled; muted.IsChecked = model.Muted;
            if (volume.Text != model.Volume) volume.Text = model.Volume;
            pending.IsVisible = model.Pending is not null;
            enabled.IsEnabled = muted.IsEnabled = volume.IsEnabled = model.Pending is null;
            var audio = model.Status;
            var config = audio?["config"];
            var state = audio?["state"]?.GetValue<string>() ?? "Unavailable";
            status.Text = config?["enabled"]?.GetValue<bool>() == false ? "Game audio disabled." : "Output: " + state
                + (config?["muted"]?.GetValue<bool>() == true ? " · Muted" : "");
            if (config?["enabled"]?.GetValue<bool>() == true && audio?["selected_listener"] is null && audio?["closing"]?.GetValue<bool>() != true)
                status.Text += "\nChoose a Game camera and press Play to hear running sounds.";
            if (audio?["error"]?.GetValue<string>() is string error) status.Text += "\n" + error;
            message.Text = model.Error ?? (model.Conflict ? "Audio settings changed elsewhere. Your edits are preserved; reload before applying." : model.Dirty ? "Unapplied audio changes." : "");
            diagnostics.Text = "Last submitted listener: " + (audio?["listener"]?.ToString() ?? "None") + "\nDevice driver: " + (audio?["driver"]?.ToString() ?? "Not opened")
                + "\nQueued ticks: " + audio?["queued_ticks"] + " · Device frames: " + audio?["device_queued_frames"]
                + "\nSubmitted frames: " + audio?["submitted_frames"] + " · Dropped ticks: " + audio?["dropped_ticks"]
                + "\nTimeline resets: " + audio?["discontinuities"];
        }
        finally { syncing = false; }
    }
    public void CloseForOwner() { ownerClosing = true; Close(); }
    internal JsonObject RenderForQualification(string destination, string projectRoot)
    {
        if (Program.Options.Script is null || !IsVisible || !IsMeasureValid || !IsArrangeValid) throw new InvalidOperationException("Audio render requires a measured script-owned window.");
        var path = Path.GetFullPath(destination);
        var project = Path.GetFullPath(projectRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (path.StartsWith(project, StringComparison.OrdinalIgnoreCase) || Path.GetExtension(path).ToLowerInvariant() != ".png") throw new ArgumentException("Audio render must be a new PNG outside the project.");
        var parent = new DirectoryInfo(Path.GetDirectoryName(path)!);
        if (!parent.Exists) throw new ArgumentException("Output directory must exist.");
        for (var check = parent; check is not null; check = check.Parent) if ((check.Attributes & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Output cannot traverse reparse points.");
        var width = Math.Ceiling(Bounds.Width * RenderScaling); var height = Math.Ceiling(Bounds.Height * RenderScaling);
        if (width is < 32 or > 4096 || height is < 32 or > 4096) throw new InvalidOperationException("Audio render exceeds size budget.");
        using var image = new RenderTargetBitmap(new PixelSize((int)width, (int)height), new Vector(96 * RenderScaling, 96 * RenderScaling));
        image.Render(this); using var encoded = new MemoryStream(); image.Save(encoded, PngBitmapEncoderOptions.Default); encoded.Position = 0;
        using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None); encoded.CopyTo(output); output.Flush(true);
        return new() { ["kind"] = "avalonia_visual_content", ["path"] = path, ["width"] = (int)width, ["height"] = (int)height,
            ["qualification"] = "Attached Audio visual content only; not an OS screenshot or listening qualification." };
    }
}
